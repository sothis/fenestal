# Fenestal: the design

The spec. `README.md` is how to build and use the library; this records the
measurements, the rejected alternatives and the reasoning behind nearly every
constant, and the sections the source cites by name (*DESIGN* in a comment)
are here.

A portable C11 library: a window, a device, a swapchain, a render thread and a
text overlay, wrapped around a scene that is the caller's. Its five
backends are Wayland, X11, KMS, Win32 and Cocoa and its four renderers
Vulkan, OpenGL, Direct3D 12 and Metal: one build per operating system,
with every backend and renderer that system has, as `libfenestal.a` and a
shared object (*One build per operating system*). `testbed/` is the
application the library is developed with, the programs under `demos/` are
the ones to start a program of your own from, and `examples/` holds the API
reference's snippets.

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

The caller's row is in the middle, not on top.

The first two rows name this document's terms. Wayland, X11, Win32, Cocoa
and KMS are the library's *backends* (`src/backend/<name>/`, the seam
`FnlBackendVtbl`, which a build has every one of for its system and
`fnl_init()` chooses among); Direct3D 12, OpenGL, Metal and Vulkan are its
*renderers* (`src/renderer/<name>/`, the seam `FnlRendererVtbl`, which a
build has every one of for its system too and `fnl_init()` chooses among
on the open window). A *platform* is one of each -- X11 with Vulkan, Win32
with Direct3D 12 -- and a build target, one per operating system (`linux`,
`win64`, `macos`), builds every platform its system has (*One build per
operating system*). A *GPU* is the device that does the drawing, hardware
or a software rasteriser such as lavapipe or WARP.

Five headers ship: `include/fenestal/fenestal.h`, the portable surface, which
includes no window-system or graphics header at all; and one interop header
per renderer, such as `include/fenestal/fnl_vulkan.h`, for what a caller
records into and what it builds pipelines against.

## Building

Requires CMake ≥ 3.18, a C11 compiler, and:

| Dependency | Provides |
|---|---|
| `wayland-client` + `wayland-scanner` | the Wayland display connection's headers, and the core protocol's XML; protocol stubs are generated from the vendored XML in `protocols/`, so no `wayland-protocols` package is needed. Opened at run time, not linked |
| `xcb` | the X11 display connection's headers — core protocol only, no `xcb-randr`, `xcb-xkb`, `xcb-icccm` or `xcb-cursor`. Opened at run time |
| Vulkan loader + headers | `find_package(Vulkan)`: the headers, which reach a consumer through `fenestal::static`; the library opens the loader at run time and hands it to a caller's scene (`FnlVulkanGpu`) |
| `glslangValidator` | compiles `shaders/*` to SPIR-V, embedded as C arrays at build time |
| `spirv-cross` | translates that SPIR-V to GLSL 4.50 and 4.10, HLSL and MSL, and the build checks each (*One shader source*); every renderer but Vulkan draws with it, so it is optional only for `-DFNL_RENDERER=vulkan` |
| `egl`, `wayland-egl`, `GL/glcorearb.h` | where OpenGL is built on Linux: EGL's and the Wayland native window's headers, opened at run time; and Khronos's header of GL's types and enums, with no function in it (`src/renderer/gl/loader.c`) |
| `freetype2` | **build-time only** — `tools/mkfont` bakes the glyph atlases with it; the program does not link it |
| pthreads | the render thread |

```sh
./build.sh                        # every tree -- thirteen, and the ARM
                                  # Linux target's four where its cross
                                  # compiler is
./build.sh linux                  # that target, release
./build.sh linux asan             # that target, that checker
./build.sh linux clang            # that target, built by clang
./build.sh linux release -DFNL_NATIVE_ARCH=ON  # for this CPU only
./build/linux/release/fenestal-testbed       # the platform it finds
FNL_BACKEND=x11 FNL_RENDERER=gl ./build/linux/release/fenestal-testbed
```

Every tree lands under `build/<target>/<variant>`. Targets are `linux`,
with Wayland, X11 and KMS and Vulkan and OpenGL in it, `win64`, with
Direct3D 12 and Vulkan, and `macos`, with Metal, Vulkan on MoltenVK
and OpenGL 4.1; variants are `release`, `clang`, `asan`, `tsan`, `analyzer` and
`gc`. Thirteen trees: six variants of the native target, three of the
Windows one and two of each macOS one, `macos` and `macos-arm64` —
mingw-w64 ships no sanitizer
runtime, so `win64` refuses `asan` and `tsan` and takes the two
link-time checkers, and it refuses `clang`, which against mingw-w64 would want
a sysroot of its own. The `clang` tree is the release build compiled by
`clang-21` (or `FNL_CLANG`) with `-Wthread-safety` and `-Werror`: the tree is
warning-clean under both compilers, and that tree is the check.

The target and the optional variant are positional and come first; anything
after them is forwarded to the configure step, so a flag on the command line
beats one the script chose. A bare `./build.sh` sweeps every tree, host target
first and `release` first within it — the cross tree borrows the host release
tree's baked atlases, so that order is what keeps the sweep self-sufficient. It
stops at the first failure. A build that succeeds is followed by
`tools/check-layering.py`, once for a sweep, and a reference that crosses the
layers fails the script with the trees already built. From clean, the
thirteen gcc trees took 125 s on the eight cores this was written on; the
clang trees have not been timed.

`CMAKE_BUILD_TYPE` defaults to `Release` and the compiler is pinned to gcc
before `project()`; `CC=clang` overrides. LTO is on for optimised builds. So
is hardening (`-DFNL_HARDEN=OFF` to leave it out): `-fhardened` where gcc
has it, and where it does not -- mingw's gcc 13, clang -- each of its parts
the compiler takes, checked by a link, and none that would add a library to the
link line. The Debug checker trees are not hardened, and the analyser tree
turns it off; what it costs in frame time has not been measured yet.
`-DFNL_VERIFY_FONT=OFF` skips the in-build bake check, which is otherwise run on
every build that re-bakes; a bake it did not check is lent to no other tree
(`build.sh`'s `font_ready()`).

**The library is compiled for `x86-64-v2`** (`FNL_ARCH`): SSE4.2 and POPCNT,
every x86-64 processor since about 2009, so a binary built on any machine
runs on every machine the project is driven on -- this Sandy Bridge desktop,
the laptops, the guests. `-DFNL_NATIVE_ARCH=ON` builds for the processor
doing the build instead, whose binary may not run on an older one; `build.sh`
passes it `OFF` for release, and a command-line `-D` wins. It was the
default, and what it bought is nothing that shows: two `pace.py` sweeps each
way on this desktop, CPU per environment averaged over the pair, every cell
at 60 fps in both:

| sizes/s | Wayland native | v2 | X11 native | v2 | win64 native | v2 |
|---|---|---|---|---|---|---|
| idle | 2.2% | 2.4% | 2.4% | 2.4% | 3.1% | 3.0% |
| 5 | 3.2% | 3.1% | 4.6% | 4.5% | 5.8% | 5.8% |
| 15 | 4.4% | 4.1% | 9.4% | 8.0% | 11.0% | 12.0% |
| 30 | 5.8% | 5.9% | 10.9% | 10.8% | 14.8% | 15.0% |
| 45 | 6.9% | 6.8% | 13.7% | 14.3% | 17.9% | 18.2% |
| 60 | 8.2% | 8.0% | 14.1% | 14.0% | 20.1% | 20.2% |
| 250 | 9.9% | 9.7% | 15.1% | 14.9% | 20.2% | 20.2% |
| border | – | – | 12.4% | 12.2% | 17.4% | 17.2% |

The largest difference, a point and a half at 15 sizes a second on X11, is
in the direction of `x86-64-v2` and is the spread between two sweeps of one
build. The frame is the GPU's and the window server's; the CPU the library
spends is in waits, not arithmetic a wider instruction set would shorten.
The hardened build is what both columns ran.

**A checker tree borrows its own target's release bake when it can.**
`tools/mkfont`'s output is the same bytes in every native tree, so `build.sh`
hands a `clang`, `asan`, `tsan` or `gc` tree the release tree's `generated/`
through `FNL_FONT_DIR` — when all four baked files are there and nothing the
bake reads (`tools/mkfont.c`, `tools/font_charset.h`, `tools/font_codec.h`,
`tools/font_raster.h`, the fonts) is newer than they are. A stale lender
would hand over an old atlas under new sources, so the answer then is no and
the tree bakes its own, which is also what keeps one checker tree asked for
on a clean checkout building unaided. The `analyzer` tree always bakes: it is
where `-fanalyzer` reads `mkfont.c` and `verifyfont.c`. `verifyfont` runs only
where the bake ran; a borrowed bake was checked by the tree that lent it.
`FNL_FONT_DIR` lends the font and nothing else, so `verifykeymap` still builds
and runs in every native tree.

Two smaller things the configure does. The LTO probe is a try-compile and
`build.sh` configures on every invocation, so its answer is cached, keyed on
the compiler and its version. And the binaries are built with
`-fmacro-prefix-map`, so `__FILE__` — which reaches the library through its
error reporting — does not carry the directory the tree was built in; debug
information keeps its real paths.

**`libfenestal.a` is one object whose only globals are the API.** The
library's objects are partially linked (`-r`) into one, and every symbol that
is not an `FNL_API` name -- read out of the headers, the same list the DLL
exports -- is made local (`objcopy --keep-global-symbols`). A program
linking `fenestal::static` therefore meets `fnl_*` and nothing else: before,
it also met some 160 internal names (`app_event`, `create_swapchain`,
`text_width`, ...), any of which a program of its own could collide with or,
worse, silently interpose. `fenestal-dev` defines five of those names
itself (`tools/drive/lifecycle.c`), so linking it at all is the check.

With gcc the partial link *is* the LTO link (`-flinker-output=nolto-rel`): the
archive carries code optimised across the library's files, as the shared
object does, and no IR. So the objects need not be fat any more, and the
archive is linkable by any toolchain; what a consumer gives up is optimisation
across the boundary between its code and ours, which the shared object never
offered either. Another compiler's tree (clang's) keeps fat objects and links
their machine code.

mingw adds one constraint: its default code model reaches extern data through
`.refptr` slots in COMDAT sections, one per name program-wide, and a slot made
local is still discarded in favour of the program's copy -- the library's
stack canary then read garbage and `__stack_chk_fail` recursed until the
stack ran out. The library's objects are built `-mcmodel=small` on Windows,
which addresses data directly and has no slots.

The public headers compile as **C11 and as C++11 through C++20**, under
`-pedantic-errors -Wall -Wextra -Werror`, which is what the `extern "C"` block
in each of them is for. C99 does not have the anonymous union `FnlEvent` is
built on and is not supported.

Clean means `rm -rf build`. `build/` is one gitignore entry for every tree, and
`build.sh` has no clean mode: deleting a tree is something to type deliberately.

### Cross-compiling for Windows

```sh
./build.sh linux                  # the host tree first, release first
./build.sh win64
```

`tools/mkfont` links FreeType and runs *during* the build, so a cross build
would compile it for Windows and then fail to run it. The cross tree borrows a
host tree's `generated/` instead, which `build.sh` wires up through
`FNL_GENERATED_DIR` and refuses to configure without — or with a bake older
than what it is baked from, the test a checker tree's lender passes, since the
cross tree cannot answer a stale one by baking its own. Shaders need no such
help:
SPIR-V is architecture-independent and `glslangValidator` on `PATH` is the
host's.

Two things are made rather than found. There is no Vulkan import library in a
Linux mingw sysroot, so one is synthesised with `dlltool` from a `.def`
generated out of the sources — a call added tomorrow is in tomorrow's `.def`
— for a caller that links the loader; nothing in this tree does.
And the Vulkan headers are symlinked into a staging directory rather than added
with `-I/usr/include`, which would put glibc's headers ahead of mingw's and
break the first `#include <stdint.h>`.

The result is `libfenestal.dll`, `libfenestal.a` and the programs, importing
Windows' own DLLs alone; `vulkan-1.dll` is opened at run time, by the library
for itself and for the examples' scenes (*One build per operating system*).

The DLL's exports are a list, `generated/fenestal.def`, read out of the public
headers at configure time: every prototype marked `FNL_API`. The attribute
would say the same and is not used, because on Windows an export is a
directive compiled into the object and the objects are the archive's as well —
a program linking `fenestal::static` would export the whole API from its own
image, and the linker could then drop none of it.

The DLL is therefore linked from the archive's one object, not from the
library's objects. Those are slim LTO bitcode in an optimised tree, and
gcc's LTO plugin does not count a `.def` entry as a reference: it dropped
every API function, and the DLL's 101 exports all pointed at address 0,
outside the image, with no error and a `.text` of 47 KB against the
debug DLL's 750. The partial link is machine code, optimised across the
library as the shared object is elsewhere, with the API its only globals.

`fenestal-testbed.exe` is linked for the **Windows subsystem**
(`WIN32_EXECUTABLE`, which on mingw is `-mwindows`), so Windows opens no
console beside the window.
It costs no entry point — mingw's `crt2.o` defines `mainCRTStartup` and
`WinMainCRTStartup` beside each other and both reach `main`. It does cost the
standard streams, which a GUI-subsystem process starts without;
`use_parent_console()` in `testbed/main.c` takes the launcher's
console, but only after checking `GetStdHandle(STD_OUTPUT_HANDLE)` is empty, so
a shell or a harness that redirects keeps its file. The subsystem is set on the
**example**, not on the library: a console tool that links `fenestal` is an
ordinary thing to want.

### Cross-compiling for macOS

```sh
./build.sh linux                  # the host tree first, release first
python3 tools/macos_sdk.py --fetch   # once: Apple's SDK, from a Mac
./build.sh macos                  # Cocoa drawn with Metal, Vulkan, GL
./build.sh macos-arm64            # the same for Apple's own processors
```

The macOS tree is built here, as the Windows one is, and the Mac only
runs what they make. The compiler is `clang-21`, the version the `clang`
trees use, and the linker LLVM's Mach-O one, `ld64.lld`
(`cmake/macos-x86_64.cmake`); nothing of Apple's toolchain is used but its
SDK -- the headers and the `.tbd` text stubs of the system's libraries and
frameworks, no code -- which only a Mac's Command Line Tools install, so
`tools/macos_sdk.py` copies one, pinned by its version, from the machine the
programs run on and keeps it under `~/.local/state/fenestal`. Building on
the Mac instead was the alternative: it would put a sweep at the mercy of
which system schenker happens to be booted into, and leave `glslangValidator`,
`spirv-cross` and the font's FreeType to be installed there as well. The font
is borrowed from the host tree as the Windows trees borrow it, for their
reason.

The target is macOS 13 on x86-64: the oldest Apple still ships an SDK that
reaches, and the last whose own drivers drive schenker's HD 630, the one Mac
GPU here. `macos-arm64` is the same tree for Apple's own processors
(`cmake/macos-arm64.cmake`, which sets `FNL_MACOS_ARCH` and includes the
x86-64 file): a Mac sold since 2020 has one, and runs an x86-64 program
only through Rosetta 2, which Apple is retiring. It is built and never run
here, with no such Mac at hand. One tree is one architecture, because the
archive is made from one module of bitcode; CI joins the two with `lipo`
into universal binaries, each program an app bundle that carries MoltenVK
in its `Contents/Frameworks`, where the library also looks for it
(`.github/stage.sh`). The two directories that speak to AppKit and Metal,
`src/backend/cocoa/` and `src/renderer/metal/`, are **Objective-C** under ARC:
neither has a C interface, and the alternative -- every call cast through
`objc_msgSend` from C -- gives up the compiler's check on every one of them.
So is the OpenGL bridge to CGL (`src/renderer/gl/cgl.m`), and outside the
library the Metal scenes and the Mac's agent (`tools/drive/mac_agent.m`).
What crosses into C state is held as an owned `void *` (`cocoa_state.h`),
because the Vulkan bridge reading it is C.

Three things differ from the Windows cross build:

- **The archive.** `ld64.lld` makes no relocatable object and there is no
  `objcopy` for Mach-O symbols, so the partial link that makes the archive's
  one object (*Building*) has no Mach-O spelling. The library's objects are
  built as LLVM bitcode instead (`-flto=full`), merged with `llvm-link`,
  every symbol but the API made internal by `opt`'s internalize pass, and
  compiled to the one object. Measured on a two-file probe before the tree
  was built this way: the API stayed global, an internal function of the
  same name as one in the program went local, and the program's own was the
  one called. The Objective-C classes go internal with the rest; the runtime
  finds them by their sections, not by symbol.
- **The blobs.** The font and the licence are `#embed`ded into C
  (`font_borrowed.c`, `font_licence.c`) rather than `.incbin`'d by assembler:
  an assembler object is not bitcode, and its symbols would be globals the
  internalize pass cannot reach. `#embed` is C23's, which clang takes in C11
  as an extension; the stub silences the extension warning for itself only.
- **Vulkan.** A Mac has none of its own. The library opens MoltenVK from
  beside the program (`@executable_path`), where `build.sh` puts the pinned
  release (`tools/moltenvk.py`), and hands its `vkGetInstanceProcAddr` to
  the examples' Vulkan scenes (`FnlVulkanGpu`); a caller that links it
  instead has a `.tbd` of `libMoltenVK.dylib` synthesised from the sources'
  `vk*` names as the Windows import library is, found at `@rpath`, the
  program's directory. There is no loader between in a tree:
  Khronos builds none for a Mac and Homebrew no longer does for an Intel
  one. A checker round has one, built here (below). The Vulkan renderer
  enables
  `VK_KHR_portability_enumeration` wherever the loader offers it and
  `VK_KHR_portability_subset` wherever the device does, as the specification
  asks of a driver that implements Vulkan on another API; neither names a
  platform, and no Linux or Windows driver offers either.

The macOS target takes `release` and `gc` (`refused()` in `build.sh`):
the sanitizers' Darwin runtimes are not part of the Linux toolchain, and
gcc's analyser does not target Darwin. `gc` is the Debug tree there, for
`FNL_ASSERT` and the fault sites, and strips by ld64's `-dead_strip`,
which `ld64.lld` takes where a GNU linker takes `--gc-sections`; Mach-O
keeps each function apart already, so it has no `-ffunction-sections`.
Its link flags name `-fuse-ld=lld` again, since a `-D` of them takes the
place of the toolchain file's `_INIT` flags, which are what choose lld
(2026-10-31, built warning-clean).

A checker round there has Khronos' loader and validation layer in front
of MoltenVK. Nobody ships them for an Intel Mac, so
`tools/vulkan_macos.py --build` builds both here, by the macos tree's
toolchain, from one Vulkan SDK release's sources pinned by their SHA-256
(1.4.363.0), into the state directory, where they survive `rm -rf
build`; the layer cross-builds whole (2026-11-01).
The library opens `libvulkan.1.dylib` beside the program before MoltenVK
(`vk_load.c`), and a tree ships none, so a release program is MoltenVK
opened directly; a checker round copies the loader, the layer and
MoltenVK's manifest beside the program, names the manifest and the
layer's directory to the loader (`LOADER_ENV`), and proves the layer
mapped by asking the agent for the process's regions (`module`). A
release round takes away what an earlier one left there. With no loader
built, a checker round has no layer to prove loaded (`NO_LOADER`, the
skip's reason) and Metal's validation alone. Metal's own API validation
is on in every checker round, which every renderer's frames there go
through -- MoltenVK's are Metal's commands, and OpenGL's are copied into
the layer by Metal. A round of a
Debug tree sends `MTL_DEBUG_LAYER=1` with
`MTL_DEBUG_LAYER_ERROR_MODE=nslog`, an error said on stderr where its
default stops the program with an assertion, and waits for Metal's
`Metal API Validation Enabled`, the round skipped as unavailable if it
never comes. Without the loader the round sends `FNL_VK_VALIDATION=0`:
MoltenVK refuses the layer it names, and says so. Measured with the
loader (schenker, 2026-11-01): `macos-vulkan-native` on the `gc` tree,
the layer loaded, 90 checks and none failed, the log clean; Metal and
OpenGL on the same tree, 90 each, none failed; and the release round
after, 90, MoltenVK opened directly and the loader's files gone from
beside it. Before the loader (the same day): Metal,
Vulkan and OpenGL rounds of the `gc` tree, 90 checks each and none
failed, no assertion and no validation error; and eight forced losses
under Metal (`device-lost%40`), eight rebuilt with none. Metal's
teardown skips its queue wait on a declared loss as Direct3D 12's did,
and needs no change: under ARC a command buffer holds what it names
until it completes, so nothing is freed under the queue.

Clang's Darwin runtime is missing for the same reason, and with it the
function `@available` compiles to (`__isPlatformVersionAtLeast`), which
the first link was refused for. What macOS added after 13 is asked of the
class or the object instead (`respondsToSelector:`), with the
unguarded-availability warning off for the lines the question guards
(`FNL_NEWER_API_BEGIN` in `cocoa.m`). The hardening probe fails on an
argument clang takes and ignores for the target, which
`-fstack-clash-protection` is on Darwin and was on every file until it did.

The public headers are compiled there as C11, as C++11 to C++20 by clang++,
and as Objective-C under ARC. The macOS tree was the first to check them
with clang++, and found `FnlEvent`'s union refused under `-pedantic-errors`:
its event structs were types declared inside an anonymous union, which C++
forbids and g++ takes as an extension, so the native trees' check, by g++,
had never seen it. They are named types now (`FnlKeyEvent` and the rest),
the layout as it was, and the `clang` trees check the headers with clang++
as well.

The first build of both trees, on its first day, came out warning-free:
`libfenestal.a` exports the 79 functions of the API and nothing of the
library's own, beside the protocol records of AppKit's own protocols, which
every Objective-C program carries the same copy of; the testbed links
AppKit, QuartzCore and Metal (or MoltenVK) and libSystem.

### Cross-compiling for ARM Linux

```sh
./build.sh linux                  # the host tree first, release first
./build.sh linux-arm64            # the linux tree for the Raspberry Pi 4
```

The Pi's tree is the `linux` tree for another processor, built here by
Ubuntu's `aarch64-linux-gnu-gcc` (`cmake/linux-aarch64.cmake`), and the Pi
only runs it. Building on the Pi was the alternative: a compiler, CMake,
glslang and spirv-cross installed on a test machine, and every build
waiting on its four cores. gcc and not clang, as the native tree is,
so the warnings the tree is clean under are the same compiler's. The
cross gcc brings its own glibc, 2.43 as the Pi's Arch is, and a program
built against it asks for no newer symbol than 2.34; a glibc newer here
than there would be the first thing to check if the Pi refuses one.

Nothing of ARM is installed beside that. What the library opens at run
time -- Wayland's, EGL's, xcb's and Vulkan's runtimes -- it wants at the
build for the headers alone, and those do not depend on the processor:
this gcc searches `/usr/include` after its own glibc's, so the host's are
found as they are, and the toolchain file lets `find_path` look there
too. Nothing in the tree links a loader -- the examples' Vulkan scenes
ask the one the library opened -- but a caller may, so one is synthesised
from the sources' `vk*` names as for Windows and macOS: a shared object of
empty functions with the soname `libvulkan.so.1`, which the Pi's own
loader answers. The build RPATH is off in the toolchain file, or every
program would name the stub's directory in this tree.

The font is borrowed as the other cross trees borrow it, and for the same
reason mkfont and the ABI comparison run in the native tree alone.
`verifykeymap` and the fuzz harnesses are built for ARM and run by none
of the build: `tools/probe/bare_checks.py` sends them to the Pi with the
corpus and runs the commands the tree wrote to `generated/checks.txt`,
the build's own arguments and counts. They are worth running there for
what differs: plain `char` is unsigned on 64-bit ARM, and the layout's
parser reads text a byte at a time. The first runs, release and `asan`
(UBSan with it, read strictly -- a `runtime error:` fails the check, where
the build's runs exit 0 past one): all five clean, and `fuzz_keymap` and
`fuzz_rules` again at thirty times the build's damaged copies, 90,000
each, clean. The tree
takes `release`, `asan`, `tsan` and `gc`: not `clang`, which would want an
ARM sysroot, nor `analyzer`, whose findings are the source's and the
native tree's already. The sweep builds it where the cross compiler is
installed and says it skipped it where not: it is the one tree whose
compiler a machine building the other ten may well lack, and the sweep
should not fail there.

The Pi 4's GPU (Mesa's `v3d` and `v3dv`) makes no OpenGL 4.1 core context,
nor 4.5, so there a program steps past the GL renderer and draws with
Vulkan. What the Pi owes is TODO.md's.

**With no monitor.** With no cable both HDMI ports read disconnected and
the KMS backend has nothing to open; `video=HDMI-A-1:1920x1080@60D` on the
Pi's kernel line forces the first on, and it then reads connected with
1920x1080, 1024x768 and 800x600. Measured by hand on the first build, all
three programs on KMS and Vulkan, on V3D 4.2 (Mesa 26.2.5): the backend
opened HDMI-A-1 on `card1` (vc4; `card0` is v3d, which drives no output)
at 59.993 Hz, and `fenestal-pace --draw` presented every frame at
16.67 ms, 60 fps, for its eight seconds -- vc4 flips and paces with no
sink. `kms_grab.py` read the testbed's frame back whole (linear,
modifier 0) once it passed over v3d's node, which refuses GETRESOURCES.
The testbed's starburst costs V3D 20.25 ms of GPU time at that size, so
an animated testbed there will not hold 60 at 1080p; `fenestal-pace`'s
frame lines put its `draw` at about 6 ms. Their `period 0.00` and `miss
0/0` are the instrument's absence, not a measurement: it runs only where
frames are invited, as on X11 and Win32 it reads the same.

**Driven.** The Pi is a bare machine (`bare_driver.py --setup pi4`), and
the harness sends each machine the tree its `uname -m` takes
(`bare_driver.TREES`): `linux-arm64` on ARM, where the Windows
environments are skipped, there being no wine. `install()` refuses a
program built for another processor and names the tree. The first
`kms-vulkan-bare@pi4` round, release, with `--capture`: 63 checks, none
failed, 7 not applicable, in 73 s -- the German layout's dead keys, the
mode rows, the moves to llvmpipe under Vulkan and under GL and back to
V3D, and the pointer seen in the scanned-out frame after each. Without
`--capture`, 56, none failed. The event queue's median is 16 ms at rest
and 45 ms with the scene moving, the starburst's 20 ms of GPU a frame at
1080p.

**Checked.** The checker trees run on the Pi's own sanitizer runtimes,
Arch Linux ARM's `gcc-libs` (16.1.1): Ubuntu's, which come with the cross
gcc, are a gcc 16 snapshot, and its ASan crashed in its interceptors' setup
before `main` on the Pi, where the Pi's runs the same program.
`kms-vulkan-bare@pi4` passed under `asan` (63 checks with `--capture`),
`tsan` (56) and `gc` (56), and `kms-gl-bare@pi4` under `asan` and `tsan`
(56 each), none failing, the validation layer loaded in each Vulkan one.
What they found was Mesa's, one of each:

- **ASan**: 156 bytes at each mode change on the display path -- the
  surface made again for the new mode, and Mesa's display WSI reassigning
  its plane's format list once the last swapchain has gone (`lsan.supp`
  has the lines; `docs/mesa-wsi-display.md`, item 3, is the report).
  `tools/probe/wsidisplay.c` shows it with nothing of fenestal in it, on
  V3DV and on aspire's anv alike. The laptops' rounds never meet it, their
  panels offering one mode; this desktop's console does, RADV's 72 bytes
  at each mode row, matched by the function's name once RADV is kept
  loaded too (`radeon` in `bare_driver.DISTROS`, where an AMD GPU is).
  Seen on the Pi once V3DV was kept loaded (`ALSO_PINNED` in
  `bare_driver.py`).
- **TSan**: the display WSI's swapchain mutex and condition variable
  destroyed against its own present thread, which `tsan-lavapipe.supp`'s
  two keys already cover on a machine with lavapipe, the Pi among them.

No race in fenestal's own code on ARM's weaker memory order, in the
rounds' reach.

### One shader source

Each shader is written once, as the Vulkan GLSL in `shaders/`. OpenGL,
Direct3D 12 and Metal get `spirv-cross`'s translation of its SPIR-V, made by
`embed_shader()` beside the SPIR-V array: `generated/gl/<stem>.<stage>`,
embedded as a C string because GL compiles text at run time,
`generated/hlsl/<stem>.<stage>`, for Direct3D's compiler, and
`generated/msl/<stem>.<stage>`, embedded as a C string as GL's is. A second
source per API, written by hand, was the alternative, and it is four copies
of every edit with nothing to say when one is missed.

Each translation is checked as it is made, by glslang, which is already a
build dependency: the GLSL by its OpenGL front end, and the HLSL by its HLSL
front end, compiled through to SPIR-V. And every GL program is linked from
its translated stages (`gl_program()`), because GL links a program's stages
at run time and a uniform block declared differently in two of them is a
link error there. The text shader declares the whole of `Push` in both its
stages for that reason.

What the renderers rely on, and so what a shader keeps to:

* **The push-constant block is named `Push`.** GL gets a std140 uniform
  block of that name (`--glsl-emit-push-constant-as-ubo`), bound by name;
  HLSL gets `cbuffer Push`, with no register, which the compiler gives
  `b0` -- Direct3D 12's root constants. std140 places a block of scalars
  and `vec2`s where std430 does, which is all a push block here holds.
* **A vertex input at location N is `TEXCOORDN`** in HLSL, which is the
  name Direct3D 12's input layout gives it, and `[[attribute(N)]]` in MSL,
  fed from buffer 1 by the pipeline's vertex descriptor.
* **`Push` is `constant Push& [[buffer(0)]]`** in MSL, in both stages, fed
  by `setVertexBytes` and `setFragmentBytes`; a binding is its own number
  (`--msl-decoration-binding`), so the one image and its sampler are at 0.
  MSL lays a struct out by its members' alignment, so a block whose largest
  member is a matrix is longer there than in C: a matrix, a light and a
  word, a lit scene's per-draw block, is 84 bytes in C and 96 in MSL, and
  is handed over padded.
* **Clip space is Vulkan's.** The GL renderer draws every frame into
  images of its own, the window's included, with no flip anywhere, so row 0
  of each is the top, as in Vulkan: a viewport, a scissor, `gl_FragCoord` (which
  `spirv-cross` leaves at GL's default origin) and a texture coordinate all
  mean what they mean there, and no GLSL changes. The finished frame is
  flipped once, by the blit onto the window's own framebuffer, which is never
  drawn into. `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`, core in 4.5,
  makes the depth range Vulkan's. `GL_UPPER_LEFT` was the alternative, and
  it puts a rendered image's top in its last row, which the scene's fetch
  and MLAA would then read upside down. Direct3D's and Metal's clip space
  points Y up, and their HLSL and MSL flip it at the end of each vertex
  stage (`--flip-vert-y`); their depth range and texture origin are
  Vulkan's already.

`spirv-cross` is required by a GL, Direct3D 12 or Metal build and optional
for a Vulkan one, where the translation is unused and runs as a check that an
edit to a shader has not left the other three APIs behind; without it the
build says so and goes on. MSL has no compiler off a Mac, so its check is
only that `spirv-cross` made it; the device compiles it when it is made,
MSL 2.1 (`FNL_MSL_FLAGS`), and Metal keeps what it compiled in the system's
shader cache. An offline `.metallib` was the alternative, and only full
Xcode has Metal's offline compiler -- a 12 GB install behind an Apple ID, on
the Mac, for a build that is otherwise this machine's. HLSL is translated
for shader model 6.0 (`FNL_HLSL_MODEL`) and compiled to DXIL by
Microsoft's `dxc` for Linux (`tools/dxc.py`, one release pinned by
SHA-256, fetched once; `build.sh` names it with `FNL_DXC`), into
`generated/dxil/<stem>.<stage>.h` as a byte array. Direct3D
12 loads only DXIL a validator has signed, outside developer mode, and the
Linux release carries the validator (`libdxil.so`): measured, the
rectangle shader's container passed `dxv` with a non-zero hash, and
Windows loads them: the stress round's first runs on Windows itself
(2026-10-20, `e8e43da`) passed whole on WARP in both guests and on each
of the laptops' four GPUs, Intel's HD 530 and 630 and NVIDIA's GTX 960M
and 1050 Ti, every pipeline state built from these containers. DXBC 5.1
was the alternative, and the one HLSL compiler packaged here
(`vkd3d-compiler` 1.2) has no HLSL front end. The root signature is
serialised at run time rather than declared in the HLSL, so the shaders
stay what `spirv-cross` wrote.

Under wine, DXIL is the catch: the vkd3d that wine 10.0 bundles in
`wined3d.dll` parses the container and answers `VKD3D_ERROR_NOT_IMPLEMENTED`
(DXIL is on by default only from vkd3d 1.16), so every pipeline state is
refused. The wine environments therefore run on vkd3d-proton, with DXVK's
`dxgi.dll` beside it (`tools/vkd3d_proton.py`): wine's own DXGI makes a
D3D12 swapchain through wined3d's vkd3d, and handed vkd3d-proton's device
it reads through a null pointer (`wined3d+0x103eb4`, from
`CreateSwapChainForHwnd`). The harness stages the program with the three
DLLs in a directory of its own and loads them by override; a tree never
holds them, since on Windows a `d3d12.dll` beside the program would be
loaded in place of the system's. WineHQ's 11.0 and 11.21 run DXIL on
their own vkd3d (*Wine 11 on this desktop*), and `FNL_DRIVE_VKD3D=wine`
puts the Direct3D 12 environments on it.

### OpenGL 4.1

The GL renderer is written against 4.5 core -- direct state access,
immutable storage, persistently mapped arenas, a base instance and
`glClipControl` -- and draws with 4.1 core as well, which is where macOS
stops (TODO.md *OpenGL on macOS*). The profile is chosen at load by the
context's version (`gl_load()`, `gl_profile41`): EGL asks for 4.5 core and
for 4.1 where that is refused, and a 4.1 context draws through the 4.1
profile. A Debug tree's `FNL_FAULT=gl-41` draws a 4.5 context through it,
which is how it is driven where every device has 4.5. What 4.5 gives, by
site:

- **Direct state access** (`gl_compat.c`): every DSA slot of the table is
  a stand-in made of 4.1's bind-to-edit calls, on binding points nothing
  else uses -- a buffer on `GL_COPY_WRITE_BUFFER`, a texture on unit 0's
  `GL_TEXTURE_2D`, a renderbuffer on `GL_RENDERBUFFER` -- and on the draw
  framebuffer, saved and put back. So every call site is the same under
  both profiles, and no vertex array's state is touched but the drawn
  one's.
- **Immutable storage** (4.2's `glTexStorage2D`, 4.4's buffer storage):
  each level specified with no pixels and the levels past it cut off;
  a buffer's usage hint read off the storage flags.
- **The persistent, coherent arenas** (4.4): the core appends into client
  memory, and each flush uploads what it is about to draw
  (`gl_arena_upload()`), into the frame slot's buffer, which the slot's
  fence guards as before.
- **The base instance** (4.2) and the separate attribute format (4.3): the
  stand-ins keep each vertex array's format, binding and buffer, and at
  the draw point each instanced attribute at the flush's first instance
  and draw from 0.
- **`glClipControl`** (4.5): a second translation of the shaders,
  `generated/gl41/`, GLSL 4.10 with the depth range mapped in the vertex
  stage (`spirv-cross --fixup-clipspace`: z = 2z - w) and no binding
  qualifiers (4.20's). A `Push` block is bound by name, as at 4.50, and
  each program has one sampler, on unit 0, which is where an unbound
  sampler uniform reads, so no `glUniform1i` is needed. Both translations
  are embedded in every GL tree and checked by glslang at build time,
  each pair linked as a program (`gl_program()`).
- **Debug output and the reset status** (4.3, 4.5): loaded where the
  context lists `GL_KHR_debug`, `GL_KHR_robustness` or `GL_ARB_robustness`,
  and left out otherwise -- asked by extension and not by the pointer,
  since Mesa's EGL hands out a stub for any name.

A device with 4.1 alone is drawn on where it is the window server's own and
nothing EGL lists has 4.5; it is not offered as a move, since the list
asks each device for 4.5 (`makes_gl45()`), and stays so until a move onto
one has been seen to land. On this hardware that changes nothing listed:
nouveau's NVIDIA GPUs give way to the Intel GPU beside them, as before.

**A scene of the caller's is told.** `fnl_gl_version()` says 45 or 41
(`App::gl_version`, written at each device build, so any thread may ask),
and `fnl_gl.h` says what 41 means: 4.1 core calls alone, GLSL 4.10, and
GL's own depth range, which the scene's vertex stage maps from Vulkan's
itself -- spirv-cross's `--fixup-clipspace`, in `FNL_GL41_FLAGS`, the
cache entry a consumer's build translates with as the examples' do. A
function and not a field of `FnlGlSceneLayout`, which has no size of its
own to grow by. The testbed's scene and `demos/texture`'s are written in the
4.1 subset, which 4.5 runs too -- bind-to-edit, a vertex array described
while bound, `glTexImage2D` with one level said -- and take their
shaders' 4.10 translation where the version is 41; the snippet clears
with 4.1's `glClearBufferfv`. Driven so on the thinkpad's llvmpipe under
the override (2026-10-31), the scenes in their 4.1 subset and 4.10
shaders: the testbed's rounds on Xorg, weston and KMS with pictures, 84,
60 and 61 checks, minimal's on Xorg and KMS, 21 and 22, none failed; and
minimal's cubes read off KMS's scanout textured, lit and in depth order.
At 4.5 the same rounds pass as before. The fault site keeps the 4.5 context
and draws the library's half through the profile, and tells the scene 41 as
well (`fnl_gl_version()` says 41 there): its 4.10 shaders on the 4.5
context.

Measured on the thinkpad (2026-10-31), llvmpipe told
`MESA_GL_VERSION_OVERRIDE=4.1` beside `LIBGL_ALWAYS_SOFTWARE=1`: Mesa
refuses the 4.5 context (`EGL_BAD_MATCH`), the 4.1 one is made, and
`fenestal-pace`, which has no scene, drew at 53 to 61 fps with its size
changed 30 times a second. Mesa's override sets the GL version alone --
GLSL 4.50 and direct state access stay -- so the testbed, its scene given
the 4.1 context for the measurement, ran whole rounds through the profile:
`x11-gl-bare`, `wayland-gl-bare` and `kms-gl-bare`, 84, 57 and 55 checks,
none failed. A round drives the profile by `FNL_FAULT=gl-41` in a Debug
tree, and `fenestal-pace` by the override: the three rounds again under
`asan` with the site forced, the scene on llvmpipe's 4.6 and the library
through the 4.1 profile, 84, 57 and 55 checks, none failed but the log's,
which held the site's own lines alone. The same override on the
Ironlake GPU, whose GL is 2.1, makes it claim 4.1 and refuse every
program (GLSL 1.40 at most), which is the override's lie and no
profile's.

On a hardware GPU (2026-10-31, `1d987ac`, aspire's HD 530, iris, Mesa
26.2.6): the override gives a real 4.1 core context ("OpenGL 4.1 (Core
Profile) Mesa 26.2.6"), and `x11-gl-bare` and `wayland-gl-bare` pass
through the profile at `release`, 91 and 62 checks, none failed; with
`FNL_FAULT=gl-41` under `asan`, `gc` and `tsan`, none failed but the
site's own log lines, and in one `tsan` round on Xorg lavapipe's keyed
`_mesa_hash_table_copy()`, left unnamed that run. What the profile costs
a frame, `pace.py --no-vsync` on the same device, alternated twice: 1183
and 1191 fps at 51.8 and 48.7% of a core at 4.5, 1149 and 1171 at 47.2 and
49.5% through the 4.1 profile -- one to three percent fewer frames, about
one run's spread, and no more CPU at the testbed's load.

### OpenGL on macOS

macOS stops at OpenGL 4.1 and keeps it only deprecated, and its windows
take frames from Metal: the `macos` tree draws with OpenGL all the same,
through a bridge of its own (`src/renderer/gl/cgl.m`) and the renderer's
4.1 profile (*OpenGL 4.1*), after Metal and Vulkan in the order. A CGL
context with no view, 4.1 core and accelerated; the window's image
blitted into an IOSurface each frame, one per frame slot, bound to GL as
a rectangle texture (`CGLTexImageIOSurface2D`) and unflipped, since an
IOSurface's row 0 is its top and GL takes it for the bottom, which is the
window image's own convention; and Metal copying the IOSurface, wrapped
as BGRA8 sRGB so the copy moves the encoded bytes, into the window's
`CAMetalLayer` drawable and presenting it. The bridge says it is
surfaceless and takes the way KMS's own buffers take
(`gl_bridge_scan_begin()`, `_show()`): `nextDrawable` is the throttle,
`displaySyncEnabled` the interval, the backend's occlusion the renderer's
(`gl_bridge_occluded()`), and the Metal device the GL context's renderer
by its registry id. GL's writes are fenced and waited for before the
copy, by polling the fence (below). Rejected:
`NSOpenGLContext` on the view, whose `update` is the main thread's at
every size and races the render thread; `CAOpenGLLayer`, which draws on
Core Animation's thread; an IOSurface as a plain layer's `contents`,
undocumented and unthrottled; ANGLE or Zink, a library to ship.

The tree links OpenGL and IOSurface beside AppKit, QuartzCore and Metal,
all the system's, and carries Khronos's `glcorearb.h` and `khrplatform.h`
copied from the build machine, since Apple's SDK has neither and a cross
build's own `/usr/include` must not reach it whole.

Measured on schenker's macOS 26.7 (2026-10-31), the HD 630's driver
`4.1 INTEL-24.5.9`: 4.1 core and GLSL 4.10, with `ARB_texture_storage`
and `ARB_timer_query` among its extensions and no base instance, debug
output or robustness, so the profile's stand-ins are what draws. Its
timer queries answer 0 -- `GL_TIMESTAMP` has no counter bits and
`GL_TIME_ELAPSED` reads 0 around fifty 4K clears -- so OpenGL there has
no GPU time to give, and `fnl_frame_gpu_ms()` says none. The testbed
drew right side up and in its colours, its title "OpenGL 4.1", and
`macos-gl-native` passed whole, 87 checks; Metal's and Vulkan's rounds
beside it, 87 each, now walk their GPU box onto OpenGL as well. The first
OpenGL round lost one fullscreen toggle of eight -- the library ended in
fullscreen after an even number of F11s, which macOS drops mid-
transition, and each transition there costs a second of frames at 5 a
second -- and the run after it lost none.

What the copy costs, against Metal on the same machine (`pace.py`,
2026-10-31, `metal@` and `gl@schenker-mac`): the same frames at every
rate -- 20 and 47 fps at 5 and 15 sizes a second, 60 from 30 up, 60
drawn -- for more of a core, 8.8 to 20.4% against 4.7 to 12.4% at the
low rates, 32.5 to 73.8% against 19.2 to 57.0% at the high ones, 34.9%
against 10.5% drawing, and the border row 60 fps at 65.3% against 58 at
21.7%. Most of it is the present (2026-10-31, `1d987ac`, `--frame-times`
with `FNL_COST_PARTS=1`, the render thread's CPU clock over each span,
the window at rest): the bridge's present -- the wait on GL's fence,
`nextDrawable`, the blit and Metal's present -- takes 3.1 ms a frame, 98%
of it on the CPU, where Metal's present takes 0.04 ms; at 60 frames a
second that is some 19% of a core, and the record and submit, 0.55
against 0.22 ms, two more. The present spins, and the spin is Apple's
`glClientWaitSync` (2026-11-01, an instrumented copy of `cgl.m`, 60 fps
with the scene moving, means over 60 frames, wall and CPU): the fence
wait 4.5 to 4.95 ms of which 4.4 to 4.8 on the CPU, `nextDrawable` 0.09,
the blit 0.09 and the commit 0.03. With no wait -- `glFlush` alone --
the same time went into `nextDrawable`, asleep (4.0 to 5.3 ms, 0.09 of
CPU), but nothing then orders GL's writes before Metal's read of the
IOSurface, two queues of one GPU; it was not taken, and whether it would
be right stays unknown. Polled instead -- the fence asked with no
timeout, 0.2 ms slept between, at most 100 ms -- the wait is as long,
4.3 to 5.0 ms, for 0.24 to 0.29 ms of CPU, and that is the present now.
After it `gl@schenker-mac` keeps its frames at every rate for 5.8, 17.0,
30.0, 32.9, 41.6 and 65.4% of a core at 5, 15, 30, 45, 60 and 250 sizes
a second, against that day's three sweeps' 8.7 to 9.6, 21 to 26, 31 to
55, 35 to 37, 41 to 62 and 75 to 88%, and 13.5% drawing against Metal's
10.5; `macos-gl-native` passes, 90 checks.

### Using it from another project

`add_subdirectory()`, and one line of linking. There is no `install()` and no
package config, deliberately: one supported path that needs nothing installed,
and the only one that works for the cross target, whose Vulkan import library
is synthesised at configure time.

```cmake
add_subdirectory(third_party/fenestal)             # or FetchContent
target_link_libraries(my_program PRIVATE fenestal::static)
```

`fenestal::static` is the archive and `fenestal::fenestal` the shared object.
Either carries its own link line — the system's libraries and nothing
else, every runtime that is not the system's being opened at run time
(*One build per operating system*) — so nothing else is named, and a
caller whose own scene calls Vulkan links nothing for it either: it asks
`FnlVulkanGpu::get_instance_proc_addr` for its entries, and the Vulkan
headers come with the library. `FNL_RENDERER` is the
one cache variable a consumer normally sets, to one renderer or `all`;
`FNL_BACKEND` names one Linux backend for a build of that one alone.

The demos under `demos/` are such consumers, each complete: a folder with a
`CMakeLists.txt` that configures on its own and takes the library from
`../..`, shaders translated on the library's flags, and a scene file for
each renderer it draws with, chosen on `fnl_renderer()`; each is built
twice, on `fenestal::static` and on `fenestal::fenestal`.

```sh
cmake -S demos/texture -B build/texture
cmake --build build/texture
```

The library's own `CMakeLists.txt` adds every demo folder as well, so every
tree `build.sh` makes also builds them through the lines a consumer runs. A
path that is only documented has stopped working by the time somebody takes
it. They were a repository of their own, `fenestal-demo`, which took this
tree as a submodule and so met a change only when the submodule moved; they
came in with their history, and that repository is retired.

Three things the library does differently as a subdirectory, all of them a
project's to decide rather than a dependency's: it leaves `CMAKE_BUILD_TYPE`
alone, it does not build the testbed, `fenestal-dev` or the snippets unless
asked for by name, and it does not add the demos. And one it does for a
consumer's sake: every imported target on its link interface — `Vulkan::Vulkan`,
`Threads::Threads`, the pkg-config ones — is made `GLOBAL`. An imported target
is visible in the directory that created it and below, so without that the
consumer's own target inherits a name it cannot see and configure stops there.
That cannot show while this is the top-level project, which is one more reason
each demo configures on its own.

A cross build from a consumer's tree needs one more variable.
`FNL_VULKAN_SCAN` is a list of globs over the *consumer's* sources: the
synthesised import library carries the entry points named in the files it
scanned, and a program that records its own draws, and links the loader
rather than asking `FnlVulkanGpu` for its entries, calls ones this tree never
does.

What a consumer has to know beyond the build:

- **Input that moves the scene is drained in `on_record`.** `on_record` runs
  before `on_overlay`, so a camera turned in `on_overlay` is drawn a frame
  late — a display period of latency, in a library that late-latches to save
  less. Both callbacks are one thread, so draining in both costs nothing, and
  a waiting event brings a frame when either is registered.
- **Animation is `fnl_invalidate()` from inside the frame.** The recording is
  cached and the library cannot see a camera, so whatever is moving asks for
  the next frame from the current one, and stops asking when it stops.
  `FnlFrame::scene_time` is the library's scene clock, behind `F2` and
  `fnl_set_scene_motion()`, stopped until one starts it.
- **Per-draw data is push constants.** They live in the recording, so there is
  no buffer for a cached command buffer to name after its contents have moved
  on. 128 bytes is all a device must offer: one matrix and a light, not two
  matrices.
- **Uploads go through the library's queue from `on_gpu_created`**, which runs
  either before the render thread exists or on it. From anywhere else the queue
  is shared and wants a lock of the caller's.

Still the caller's problem, and stated rather than solved: `Escape`, `F2`,
`F9` and `F11` are bound by the library and cannot be rebound; and the
testbed and the demos are the only programs this API has met.

### The API reference

`build/api/index.html`, written by `tools/mkapidoc.py` after every build
that succeeds, is the reference: one page, a section per section of the public
headers, every declaration with the comment above it and its thread, and
the examples. It is made from the headers and nothing else, so it says what
they say; the headers stay the authority.

The examples are `examples/*.c` -- a window with text in it, the
event queue, a text field with copy and paste, a titlebar of the caller's
own, and the rest `README.md` lists, down to a scene recorded through each
renderer's seam -- each a complete program that every tree with its API
compiles and links (`snippet-*`), so an example that stops
compiling fails the build rather than its reader. Whole programs are the
demos.

The same pass is a check, and fails the build: every exported function has
a comment, a thread tag (`[main]`, `[any]`, `[overlay]`, or `on_record`), and
a caller somewhere the build compiles.

### Compatibility

Fenestal is 0.9, and nothing the public headers declare is
promised yet: the API is still moving, and a promise made now would hold back
the changes that 1.0 should come after. A minor release may rename, change or
remove anything, at the source and at the binary, and a patch release does
neither. Two things keep a mismatch loud rather than silent: `fnl_init()`
compares the major and, below 1.0, the minor of the header the caller was
built with, and the shared object's soname carries the minor
(`libfenestal.so.0.9`), so a program linked against 0.9 is not loaded by
0.10.

The header's *stability* block says what 1.0 will promise, and the tree
already works that way where it costs nothing: enum values are appended and
not renumbered, the descriptors a caller fills grow at their end behind
their `size` field, and a replaced function is `FNL_DEPRECATED` before it
goes. Outside it, then as now, are the fields marked `UNSTABLE` --
`scale_override120`, `gpu_index` and `late_latch_fixed`, which are test
fixtures -- and the text the two diagnostics print.

Three checks run in every build rather than in review (the ABI's in every
native one: a cross build cannot run the dump):

- **The ABI against a baseline.** `tools/abidump` prints every public struct's
  size and offsets and every enum constant, and the build compares that and
  the export list with `tools/abi/`. A difference fails the build with the
  `diff` to read. An intended change is made by copying the new dump over the
  baseline in the same commit, with a line in `CHANGELOG.md`; the check
  makes every change a decision, and from 1.0 it will allow only additions.
- **The export list.** The archive's globals and the DLL's exports are the
  `FNL_API` prototypes, read out of the headers; the shared object's symbols
  are versioned `FENESTAL_0`, one node until 1.0, when each minor release
  that adds gets a node of its own (`fenestal.map`).
- **The headers alone**, compiled as C11 and as C++11 through C++20, pedantic
  and warning-free (`tools/headercheck.c`).

The version is said in two places, `FNL_VERSION_*` in the header and
`project()` in `CMakeLists.txt`, and configure refuses them disagreeing.
`CHANGELOG.md` is what each release changed.

Toolchains: gcc, which the build defaults to, and clang, which the `clang`
trees build warning-free, on Linux; mingw-w64's gcc for Windows. MSVC is not
supported and no support is planned: the MSVC branches in `core/compiler.h`
are written and have never been compiled.

## The testbed

| Input | Action |
|---|---|
| `W` `A` `S` `D` / arrows | held-movement bits (plumbed, nothing moves yet) |
| `Shift` | run modifier |
| Left click in the presentable area | captures the pointer |
| `F1` | keybindings panel and status lines |
| `F2` | set the test pattern moving |
| `F3` | text entry on and off (see `--echo-events`) |
| `F4` / `F6` | copy a fixed string / ask for the clipboard |
| `F5` | the library's metrics panel, shown from the start here: FPS, frame cost, input latency, this process's CPU, RAM, VRAM and GPU use |
| `F7` | ask for the output after the window's own (`fnl_request_output()`): how a fullscreen window is moved, the bar not being shown there |
| `F9` | release the pointer |
| `F11` | toggle fullscreen |
| `Esc` | quit |

The titlebar is the library's, and so is everything in it: four window buttons
(minimize, maximize, fullscreen, close), a resolution drop-down, three
checkboxes — `FSAA`, `MLAA`, `Fs Upsample`, in the order the pixels go through
them — and a GPU drop-down that moves the window onto another GPU or renderer
while it stays open, dim where there is only one, and before it, where the
window is wide enough, an output drop-down naming the monitor the window is
on and moving it to another, dim where there is one monitor and on
Wayland, whose clients place no window.
Double-click to maximize, drag to move. The testbed draws none of it; see
*The titlebar controls*.

### The metrics panel

F5's panel is the library's (`src/core/metrics.c`), on every renderer and
in every program: hidden at start, toggled by a key the caller chooses
(`fnl_set_metrics_key()`, F5 until set, none on `FNL_KEY_UNKNOWN`, and
a value no `FnlKey` has ignored, by `echo_key_known()`) and
shown from code by `fnl_set_metrics_panel()`. It was the testbed's, drawn
through the public overlay, and moved into the library so a program
gets it without copying the testbed: the frame's figures were public
already, and what was not -- the process's CPU and memory, and the GPU
memory, which only the renderer can ask its API for -- is now sampled by
core behind `#ifdef`s, as signals are, and by a seam entry
(`FnlRendererVtbl::gpu_memory`).

Every figure it shows is in the draw state, so it is drawn from what the
frame latched and a figure that moves re-records the frame as fps does,
with no `fnl_invalidate()` anywhere. The process's figures are sampled on
the render thread at most once a `METRICS_WINDOW` (half a second: a share
of CPU over a few frames is noise) and only while the panel is shown,
and the two shares, the CPU's and the GPU's, start again from the moment
it is shown. Hidden it costs nothing.

**Where the sample is taken.** After the present, where the window is
drawing, and latched by the next turn; before the draw state only at
rest, where no hold is run. Before the draw state on a drawing window,
it would fall past the pacing's hold and before `FRAME_MARK_REC`, so
outside `late_cost`: a part of the frame the late latch never budgets
for, twice a second -- `getrusage()`, `/proc/self/statm` and the GPU
memory, which under OpenGL is NVML's query (half a millisecond,
*Benchmarking*) or a read of every descriptor's
DRM fdinfo, twice on Intel where i915 finds none. After the present it
is outside the frame, between a present and the next latch: where frames
are invited (Wayland) it falls in the wait for the next invitation, and
where they are not the backend's wait answers at once, so the hold after
it starts that much later -- the latch moves, the frame's own budget
does not. A figure the panel was hidden with is not shown again: hidden,
the four read none, so a panel shown on a drawing window has dashes for
the one frame before its first sample, and the two shares until the
second, which is the first with something to measure from.

**Shown, it keeps nothing measured at rest.** Sampling at rest as
`fnl_set_cost_sampling()` does -- a full frame every `COST_WINDOW`, each one
counted -- has a still window draw five frames a second for the panel alone,
30 % of a core on lavapipe against 0.2 to 0.4 % with it hidden (*Measured at
rest only where the caller asks*), and makes those frames what its figures
measure: the rate reads 5, the sampling's own, and the process's CPU is mostly
the panel's. So the panel asks for a frame at rest only where its process
figures move, at most once a `METRICS_WINDOW`, and that frame is drawn for the
figures alone (`App::cost_counts` false): it owes no publish and its times stay
out of both means -- the GPU's by the frame's index, since its time is read
frames later (`App::gpu_uncounted`), and the CPU's at the present -- while the
frame-stats totals and the ring's rows keep every frame. So at rest the frame
rate and the two costs are the last drawn frames', as they are with the panel
hidden, and the process's share of a core is the window's at rest and at most
two frames a second. Such a frame is drawn with the window left at rest
(`rest_owed()`), as a sampled one is: woken, the next turn would run the pacing
for a frame it does not draw, and on X11 the display hold would read each of
them, a fifth or a half of a second apart, as a missed frame and deepen the
queue it keeps shallow. Where the thread's CPU time is not known (Windows under
wine) the CPU figure is the wall clock over the counted frames alone
(`App::wall_cost`), not `late_cost`, which the pacing keeps over every frame. A
caller that wants the frame's figures live at rest still asks
`fnl_set_cost_sampling()`, and the panel's frames then count as every sampled
one does.

Measured on 2026-11-05, `--frame-times` for the frames and this process's CPU
read off `/proc` (the testbed now echoes the scene's motion, so the window is
known idle): at rest on the RX 590's Vulkan the panel costs 0.25 % of a core
shown against 0.00 % hidden, drawing 13 frames in 8 s (1.6 a second); on
lavapipe ~30 % against 4.5 %, 25 frames in 12 s (2.1 a second), the hidden
figure being llvmpipe's own workers at idle, which drew two frames in the
12 s. A minute at rest on X11 with vsync, the panel up: 88 frames over 88
`frame at rest: N drawn` lines (1.47 a second) and not one frame line, 0.38 %
of a core; the scene moving then gives `miss 0/0` and `hold 0.00`, so the
display hold never deepened. Under OpenGL on amdgpu (fdinfo), late latching
on and the scene moving, no vblank is missed with the panel shown or hidden
and no frame line reports one, the shown mean 16.06 ms against 16.57 hidden:
the sample after the present costs nothing. Under wine, where the thread's CPU
time is not known, `--frame-cpu-log` -- and not `--footprint-log`, which asks
for cost sampling and so counts the panel's rest frames -- shows the
per-frame CPU held at 0.35 ms through a rest of nine panel frames in 12 s
drawn for the figures, with no frame line: the last moving frames' figure,
not the panel's own. Under OpenGL on NVIDIA (NVML, schenker's GTX 1050 Ti
under weston, the scene moving) the panel adds nothing: 1120 vblanks missed
on 39 frame lines with it shown against 1190 hidden, means 16.80 against
16.81 ms -- the misses are the offload path's own. `--frame-times` with the
testbed's `--frame-stats-log` (X11, RX 590, 2026-11-05): at rest from
startup the panel drew a rest frame a half second and the totals counted
every one -- frames and rows both 124 to 127, GPU 35621 to 36830 us, 403 us
a frame -- while the two figures held at the last drawn frame's (CPU 0.24,
GPU 0.39 ms); moving, the drained rows lag the totals by the newest frame or
two (370 against 369 frames, 115800 against 116768 us at one line); and at
rest a rest frame's GPU time reaches the totals when it is read, after the
sample's quarter-second hold is out, so the row drained for it says -1
(`fnl_frame_samples()`).

Its place is the presentable area's top right, under whatever bar there
is, and nowhere where the window cannot hold it whole: the library does
not know the caller's own panels, so it is drawn over them rather than
around them -- after the caller's overlay, flushed first so its
background covers the caller's glyphs as well, and under the library's
drop-downs, whose lists open over the same corner. Its colours are the
controls' box, line and dim, with the frame's halves in red and green
and the rate yellow (`COL_MET_*`). In the testbed it sits beside F1's
keybindings, 316 points across against their 236, so the two
meet only in a window narrower than some 588 points, where F5 now covers the
right of F1's rather than going under it as the testbed's own did.

GPU memory is the API's own figure where it keeps one -- Vulkan's memory
budget, Direct3D 12's `QueryVideoMemoryInfo`, Metal's
`currentAllocatedSize` -- and under OpenGL, which keeps none, the
system's on Linux: NVIDIA's NVML, opened at run time, or the kernel's
DRM fdinfo for i915, xe and amdgpu (`src/renderer/gl/memory.c`). NVML
holds NVIDIA's driver while it is started, which would stop a move to
nouveau (*Another kernel driver, with the window open*), so it starts
only when the panel first asks and is shut down with the device, which
every move rebuilds.

Its GPU row is the share of the GPU's time this program's frames take:
the GPU's seconds on every frame read over the sample's window, by the
window's length (`App::met_gpu_s`), so it is small for a light scene on a
fast GPU, not most of the frame. Sampled as the CPU share is, and not
`fnl_frame_gpu_ms()` times `fnl_fps()`, which it equals while frames
flow evenly: those two stand at the last moving frames' figures through
a rest (*Shown, it keeps nothing measured at rest*), and their product
would go on saying a still window keeps the GPU busy. Every frame read
counts, those drawn only to show the figures as well, since they are
what the window takes of the GPU at rest. Measured as the product, with
the pattern moving on schenker's HDMI at 3840x2160@30: 2.3 % on NVIDIA's
OpenGL (0.76 ms) and 22 to 25 % on Intel's Vulkan, each what the
ranking's figure for that entry gives at 30 frames a second (1293 and
122 fps), and 50 % on llvmpipe (16.6 ms). The row itself, on schenker's
Intel Vulkan maximized on that monitor (3840x2117, 30 fps, 2026-11-05):
18.9 %, the product 6.30 ms by 30 fps over 10, 18.9 %; at rest with the
panel shown and the scene still, 0.0 % where the product stands at the
last moving frames' 18.9 %.

On the Mac (2026-10-31, `1d987ac`, the testbed at rest with
`--footprint-log`, the Mac's own `ps` of the same process beside it) the
RAM row is `task_info`'s resident size, within a MiB of `ps`'s RSS under
all three renderers, and the CPU row the same order as its `%cpu`. Metal's
VRAM is `MTLDevice.currentAllocatedSize`, 31 MiB, and MoltenVK's 36 to 42;
OpenGL there has neither VRAM nor GPU, both dashes, since CGL keeps no
figure for a process and its timer queries answer 0 (*OpenGL on macOS*).
On both laptops' Windows (the same day, each renderer on each GPU,
Windows' own counters of the same process beside it): the RAM row within
half a MiB of the working set; Direct3D 12's VRAM, `QueryVideoMemoryInfo`'s
local group, within 1.4 MiB of the dedicated counter on NVIDIA and of
the shared one on Intel, whose UMA "local" group is its share of system
memory; Vulkan's, the memory budget's heap usage, 6 to 11 MiB above
Windows' counter on all four GPUs through DXGI, and within 1.4 to 2.8
MiB of it on the GPU's own swapchain (`FNL_VK_DXGI=0`, aspire): the
window's images through DXGI are Direct3D 11 textures shared by NT
handle and imported into Vulkan, which Vulkan's budget counts and
Windows' per-process counter does not -- about two images at
1280x720. The CPU row moves in steps
of some 2.5%, the process times' 15.6 ms tick over the panel's window,
and averages to `Get-Process`'s.

| Option | Effect |
|---|---|
| `--no-late-latch` | sample each frame's state at the top of its turn |
| `--late-latch=<ms>` | hold the delay at `<ms>` rather than sizing it per frame |
| `--scale=<n>` | run at surface scale `<n>`, 1 to 8, instead of the desktop's |
| `--gpu=<n>` | run on device `<n>`, one-based over the devices that can present, in the order the driver enumerates them — the numbering the startup log prints |
| `--gpu=<name>` | the same by any part of the device's name, letter case aside (`--gpu=intel`); `FNL_GPU` in the environment is read the same way by every program of the library's |
| `--layout=<spec>` | on KMS, type through this layout instead of the console's: `de(nodeadkeys)`, `de,us` for two groups, the XKB options after a space; `FNL_KMS_LAYOUT` in the environment is the same, and is KMS's alone — see *KMS: no window server* |
| `--gpu-driver=<d>` | on KMS, offer a move of an NVIDIA GPU between nouveau and NVIDIA's driver with the window open, an entry in the GPU box for each renderer; `nouveau` or `nvidia` binds that one before the first frame, `switch` leaves it as it is; `FNL_GPU_DRIVER` in the environment is the same — see *Another kernel driver, with the window open* |
| `--gpu-rank=<r>` | the GPU box ranked by each entry's measured frame rate, kept in the user's cache, best first, every start opened on the first, and measured for an entry and display not measured before; `calibrate` measures every entry on this display again, `keep` uses it and never measures, `order` orders the box by it and starts as without, `off` uses none; `FNL_GPU_RANK` in the environment is the same — see *Ranked by measured frame rate* |
| `--transition=<ms>` | how long the first frames after a move to another GPU, renderer or kernel driver dissolve from the last frame before it; the library's own is 250, and 0 is a cut — see *The veil, and what the screen shows across a move* |
| `--custom-titlebar` | draw the titlebar in the testbed instead of the library — see *Replacing the titlebar* |
| `--frame-times` | where each frame's wall clock went, a line each half second on stderr |
| `--no-vsync` | draw flat out, for measuring; `FNL_VSYNC=0` in the environment does the same for any program (*Vsync and present modes*) |
| `--echo-events` | print every event the library delivers, for a test harness — the library's own echo, which `FNL_ECHO_EVENTS=1` switches on for any program built on it |
| `--text-input` | text entry on at startup: with `--echo-events` each character a key types is a line of its own, a held key repeats, and Escape no longer closes the window, so close it from the titlebar — see *Text entry* |
| `--footprint-log` | this process's CPU, RAM and VRAM, the metrics panel's figures, on stderr twice a second as `footprint: ...`, for a harness that cannot read the screen — see *The metrics panel*. It asks for cost sampling, which counts the frames the panel draws at rest |
| `--frame-cpu-log` | the per-frame CPU figure (`fnl_frame_cpu_ms()`) on stderr twice a second as `frame cpu: ...`, without asking for cost sampling: the panel's own figures at rest — see *The metrics panel* |
| `--frame-stats-log` | the frame loop's totals (`fnl_frame_stats()`) and the rows they are made of (`fnl_frame_samples()`) on stderr twice a second as `frame stats: ...`, with the two figures the means are kept from; turns frame-stats counting on — see *The metrics panel* |
| `--bench`, `--bench-run`, `--bench-<key>=<value>`, `--bench-replay=<record>`, `--bench-list` | the benchmark mode — see *Benchmarking* |
| `--licence` | the Ubuntu Font Licence and copyright notice the baked glyphs are under (`--license` is accepted too) |
| `-h`, `--help` | usage |

### Benchmarking

`fenestal-dev pace` measures the library's floor; the testbed's benchmark mode
measures a frame with an application in it, under options every one of which
is written down, and writes one JSON document per run
(`testbed/bench*.c`; the plan and its later phases are
`docs/benchmark-mode.md`).

**Each window is a lifetime.** The setup panel, every run and the results
are each `fnl_init()` to `fnl_shutdown()`, because half the options are a
window's at birth -- its size, scale, vsync, backend, renderer and GPU --
and because a window that ran before carries its history into the next run:
a tuned pacing controller, a GPU ranking that has already moved it. A
repeat is a run again, in a window of its own, and the repeats of one start
share a `series` id.

**One table, four readers.** `BENCH_OPTIONS` is the command line
(`--bench-<key>=<value>`), the panel's rows, the record's `options` object
and what a replay reads back; precedence is the defaults, then the replay,
then the command line, then the panel. A record holds every option, so a
replayed one at its default counts as not given, as it was not in the run
the record is of: the panel shows it as a default, and a priority at its
default is not asked of a process already nicer, which the system refuses.
The testbed's own options that a row sets in another way (`--scale`,
`--gpu`, `--no-vsync` and the rest), and `--footprint-log`, which a run
never reads, are refused beside `--bench`: a record that did not say what
it ran under would not be one. The exit status is the worst of every
series the program ran: 1 for a run that could not start or a record not
written, else 2 for a run that did not complete, else 0.

**What is measured, and where.** A run's clock is a thread of the
program's, as `pace.c`'s is: it holds the settle (4 s: the pacing's
controllers wait out sixty presents), the warm-up, the measure and the
cool-down, publishes the phase in an atomic, and ends the window with
`fnl_quit()`. Everything else is measured on the render thread, in the
callbacks: an interval is the time between two `on_overlay` calls, which
is the frame loop as an application sees it, and the scene's `on_record`
and the bench's own `on_overlay` are timed around themselves. Each
interval-ms the library's own figures (`fnl_fps()`, the two frame costs,
the input latency), the process's resident set and the GPU memory are a
row of the series; the process's CPU is the difference of its user and
system time across the measure's two edges. Under `redraw=every-frame` the
overlay asks for the next frame from inside the frame (`fnl_invalidate()`),
so the window never rests; `at-rest` asks for none and reads the cost
sampling's five frames a second.

**What makes a run invalid, and what is only noted.** A run is `invalid`,
with each reason named, when something changed what was being measured
under it during the warm-up or the measure: the focus lost, a resize, a
scale, a window state or an output that was not asked for, a move to
another GPU, a device lost, events dropped, the present mode changing, the
scene's motion toggled by `F2`, a gap of more than three intervals between
two rows, or the GPU ranking measuring the devices (fenestal.h,
`FnlInitDesc::gpu_name`) -- which the run waits out when it starts during
the settle, and restarts the settle after. Escape or a closed window is
`aborted`. What is only noted: no input during the measure (so no
latency), GPU memory the system will not say, a vsync asked off that the
present mode does not show, and a series that reached its cap
(`BENCH_SAMPLES_MAX` values; the frame loop's rows, twelve a frame, reach
it after some 87,000 frames) or ran out of memory, whose figures then
cover only the measure's start.

**The record**, `fenestal-bench/1`: keys end in their unit, a metric is
`{"value": x}` or the seven-figure distribution `{n, mean, min, p50, p95,
p99, max}` (nearest rank, so every figure was measured), and unknown is
`null`, never 0. It goes to `$XDG_STATE_HOME/fenestal/bench` (or the
platform's state directory, or `--bench-out`), one line on stderr says
where, and `--bench-emit` writes it whole on stderr between marker lines,
which is how it comes home from a machine reached only by a shell -- the
harness reads stderr alone.

**The frame loop's own account** is the record's `metrics.library`: the
totals of `fnl_frame_stats()` read at the measure's two ends and divided by
the frames drawn between them -- pace, fence, acquire, record, submit,
present, rebuild and draw per frame, the GPU's time per timed frame, the
share of frames whose slot the GPU had finished, and the display's cycles
and misses where the window server invites frames (Wayland; `null`
elsewhere, which is not the same as none missed). `scene-motion` runs the
scene's clock (`fnl_set_scene_motion()`), so every frame records again
rather than replaying its recording.

**The machine's conditions are options too** (the Load page, `bench_load.c`):
threads of background CPU load, each busy for its duty of every 10 ms slot,
from the warm-up to the end of the cool-down; the CPUs and nice of that load;
and the CPUs and nice of the whole program, set on the main thread before
`fnl_init()` so that every thread the library starts inherits them. The
record's `conditions` object is what was got, read back -- a nice below the
process's wants a privilege a desktop session seldom has, and a refusal is a
note, not a failure -- as is a raised nice the run cannot set back after,
which the runs after it in the same process then keep. On schenker, flat out
on Xorg (1445 fps, p99 0.81 ms, 1 % low 1184 alone): eight threads of load on
its eight CPUs leave the rate at 1424 and take the tail to p99 2.91 ms and
a 1 % low of 317; the same load at nice 19 gives 1572, 0.87 ms and 571; and
the load on CPUs 0-3 with the program on 4-7 gives 1627, 0.68 ms and 1398 --
faster than with no load at all, the busy cores plausibly keeping the package
out of its deep idle states. A load is not only a cost, which is why it is an
option and not a fixture.

**Each system sets what it has.** Windows has a mask of CPUs and a
priority class for the process, and a mask and a priority level for a
thread: the nice an option holds is mapped onto the nearest of the five
classes (idle 19, below normal 10, normal 0, above normal -10, high -20;
not realtime, which wants the administrator and starves the window
server's own threads), and the record says the nice of what was got. A
thread may only go on CPUs its process may use, and Windows refuses the
rest where Linux, whose masks are each thread's, does not: the load on
CPUs 0-1 with the program on 2-7 is refused and noted there, on 2-3 it is
granted. macOS has a nice for the whole process and no affinity at all,
so the program's nice is set and the rest refused and noted. What the
record read back on schenker under Windows is itself a finding: a
program the agent's scheduled task starts runs at below normal, nice 10,
unless the run asks for another class.

**The driver drawing** is the record's `platform.driver`: Vulkan's
`VK_KHR_driver_properties` name and version text where the device lists
it (chained only then: a struct of an extension it lacks is invalid), the
API version and the PCI ids; OpenGL's vendor, renderer and version
strings, which name no PCI ids. Direct3D 12's is DXGI's: the adapter's
description, which names the device and not the driver ("NVIDIA GeForce
GTX 1050 Ti", "Microsoft Basic Render Driver" for WARP), the user-mode
driver's version (32.0.15.8278), which `CheckInterfaceSupport` answers for
`IDXGIDevice` alone, the highest feature level the device reaches and the
PCI ids. Metal's driver is the system's and has no version of its own, so
it is the device's name, macOS's version and build (26.7.1, 25G241), the
newest GPU family the device is in, and no PCI ids.

**What the bench costs the frame it measures.** Its `on_overlay` is
0.01 ms a frame on schenker, and the frame a row is taken in up to 0.5 ms
more -- for OpenGL the GPU memory is the system's, NVML's on NVIDIA's
driver (`gpumem.c`), and its query is that half millisecond. The first
query opens NVML, which took 20 ms of one frame: a 50 ms interval at the
first row of every OpenGL run on weston, frame 60 of each, until the first
query was moved into the settle. What is left there is the renderer's own:
OpenGL on weston on the 1050 Ti misses one or two vblanks in 300 frames
(33 ms, the bench's callback under 0.6 ms in them), where Vulkan misses
none.

**Each drawn frame's own phases** come from the library's ring
(`fnl_frame_samples()`): the frame loop keeps the last 1024 drawn frames'
pace, fence, acquire, record, submit, present, rebuild and draw, pushed with
the totals' publish so it costs no lock of its own, and the bench drains it
every measured frame. The record has the distribution of five of them in
`metrics.library` -- `frame_pace_ms`, `frame_acquire_ms`,
`frame_present_ms`, `frame_draw_ms` and `frame_gpu_ms`, the GPU's time
being a column of the ring's too -- and, with the `frames` option, every
row in `library_frames`. On schenker the rows were the measure's 300 frames with
contiguous indices and none lost, and their means are the totals' to the
microsecond. What the means hid: Xorg's present call is 4.1 ms on average
and up to 6.4, and KMS's 3.8 ms is in the acquire instead.

**The GPU's time is a column of the ring too** (`FnlFrameSample::gpu_ms`),
though every renderer reads its timestamps frames after it drew them:
each names the frame it timed as it writes the pair (`app_gpu_timed()`,
the frame's index) and hands that back with the time once read, and the
render thread folds the times read in a turn into the rows still in the
ring at that turn's publish. A row the GPU timed waits for its time and
holds back the rows after it, so a caller still reads them in the order
drawn -- for `GPU_HOLD_FRAMES` frames drawn after it or `GPU_HOLD_S`
since its latch, the poll having read one two or three frames later --
and the loop's end lets every row go. So the bench no longer keeps a row
by when it drained it: it counts the rows handed over, the lost
included, and keeps those whose place falls between the measure's two
readings of the totals (`FnlFrameStats::frames`), draining on through the
cool-down and once more after the loop, which keeps the rows' means the
totals'. On schenker every measured frame had its time, on Vulkan,
OpenGL, Direct3D 12 and Metal alike, and the rows' mean was the totals'
to the microsecond (OpenGL 1.64357 ms against 1.64359). The output's
refresh rate is `fnl_output_refresh_mhz()` and the record's
`platform.refresh_hz` -- on Win32 the display configuration's signal
(`QueryDisplayConfig`'s `vSyncFreq`, 60.02 Hz on both laptops), which the
frame loop now paces on there as elsewhere until it has measured the
period.

**When a frame reached the screen** is the display's own stamp, where the
backend hears one (`fnl_frame_shown()`, a ring like the frames'): KMS's
page-flip event, which carries the frame's index out as the flip's
`user_data` and back with the kernel's timestamp, for the flips the library
makes -- the copy path and OpenGL's bridge; and Wayland's `wp_presentation`, a
feedback asked for just before each present so it rides on that commit,
answered on the main thread, sixteen slots in the backend's state and no
allocation, and a present that never committed voiding its slot, since its
feedback would otherwise stamp the next commit's frame. Both are kept on
CLOCK_MONOTONIC alone, the frame clock's, so no clock is converted. And
X11's Present extension, whose completions the backend selects on its
window (`x11_present_open()`, encoded by hand, the Generic Event
Extension's version asked first as its events need): the graphics API
presents on a connection of its own -- Mesa's WSI and EGL, by
PresentPixmap -- with that connection's serials, so a completion is the
oldest frame not yet answered, queued at `frame_request()` and taken back
by `frame_cancel()`, one completion a present and in order; a skipped one
stamps nothing, and a driver that presents some other way answers none,
the queue then letting its oldest go at sixteen. The stamp is the
server's `ust`, CLOCK_MONOTONIC's microseconds on Linux -- and a stamp
from before the frame was presented is none: a present the server
carries out at once (vsync off, or Mesa's `vblank_mode=0`) completes with
the vertical blank before it, which read as every frame on the screen a
millisecond before its latch, so the backend keeps each frame's push time
beside it and stamps nothing older.

**And where the driver or the system says so instead.** Vulkan's own
display path on KMS, whose flips Mesa makes, by `VK_EXT_present_timing`:
the KMS bridge asks for it (`VkBridge::driver_stamps`), the device
enables it with `present_id2` and `calibrated_timestamps` where it has all
three (Mesa 26.2's Intel driver does; NVIDIA's 580 has none), the
swapchain is made with it where the surface reports the first pixel out,
and each present the caller counts asks when its first pixel went out,
read back without waiting a frame later. Mesa's display path lists no
CLOCK_MONOTONIC domain, only the present stage's own clock, so that is
calibrated onto CLOCK_MONOTONIC by `vkGetCalibratedTimestampsKHR` at the
swapchain and again each second (`TIMING_CALIBRATE_S`). On schenker's
HD 630: every frame stamped, 16.661 ms apart on a 60.020 Hz panel, 20.7
ms from latch to screen with a 1.1 ms spread -- Mesa's display path keeps
a frame queued, where the copy path's own flips show one 6.5 ms after
its latch. Win32 by DXGI's frame statistics, for Direct3D 12 and for
Vulkan through DXGI: each present's number (`GetLastPresentCount`) is
named with the frame's index, and each frame the newest number shown is
stamped with its vertical blank's QPC time, now_seconds()'s clock there
(`app_present_numbered()`, core's, which names no API). Only the last
frame shown is said, so a frame that came and went between two looks has
none; Direct3D 12's statistics are not asked while its shown waiter asks
them. A stamp from before its present is dropped here too: WARP in the
Windows 11 guest stamped frames minutes before they were drawn, and its
records have none. Metal by each drawable's presented time, whose handler
runs on a thread of Metal's and can outlive the App: it reaches no App,
only a box the blocks keep alive, which the render thread empties into
the ring once a frame; its clock, CACurrentMediaTime()'s, stops while the
machine sleeps, so it is moved onto CLOCK_MONOTONIC by the two read
together. Vulkan on the Mac by `VK_GOOGLE_display_timing`, the older
extension, which MoltenVK takes from the same drawables: it names no
clock, so the bridge that knows whose driver it is converts
(`VkBridge::driver_clock`; MoltenVK's is `mach_absolute_time()`'s, moved
onto CLOCK_MONOTONIC as Metal's is), and it is asked for only where
`VK_EXT_present_timing`, which names its clock, is not to be had. A
Vulkan swapchain of Windows' own (`FNL_VK_DXGI=0`, or a device that
shares no image with Direct3D 11) says nothing: NVIDIA's 582.78 and
Intel's drivers there offer neither extension, only `VK_KHR_present_id`
and calibrated timestamps. Through DXGI, the default where the device
shares, the 960M is 20.1 ms from latch to screen, every frame stamped.

| latch to screen p50, 60 Hz | |
|---|---|
| Win32, Direct3D 12, the four laptop GPUs | 19.8-20.3 ms |
| Win32, Direct3D 12, aspire's HD 530 in a window, its own swapchain | 36.9 ms |
| Win32, Vulkan through DXGI, the four laptop GPUs | 20.1-20.3 ms |
| macOS, Metal, schenker (HD 630) | 69.4-70.2 ms |
| macOS, Vulkan on MoltenVK, schenker (HD 630) | 65.0 ms |

**Direct3D 12's frames waited a refresh in the library, and wait one in
DWM where it composes them.** With vsync on its swapchain is made without
`ALLOW_TEARING` (`d3d_bridge_swapchain()`; vsync is the window's for its
life): the flag, there for vsync off, cost aspire's 960M a refresh, 53.1-53.2
ms against 36.4 in two runs each, and changed nothing elsewhere. PresentMon
2.6.0's trace (2026-10-30, eight seconds of the bench each, the program
alone) showed every path presenting about 0.3 ms after a refresh, its GPU
done 1 to 4 ms later, and Direct3D 12's frame shown a refresh after a D3D11
swapchain's in the same mode -- through a D3D11 swapchain of its own as
well, so not Windows' present. It was the frame-latency object a count
ahead: the wait the first frame skips was taken back by a poll that comes
before the count does, every time, so two frames stood queued for the
window's life (`d3d_draw_frame()`, the take-back). One count taken once
took the refresh off on the 960M (36.4 to 19.8 ms); the take-back waits
now. The other refresh was DWM composing what it could have flipped, and
the cause was the library's again: a window's buffers are larger than the
window while its size changes (*A swapchain larger than the window*), and
on Win32 they stayed so. Windows flips onto a plane only a buffer the
window's size, fullscreen as well as in a window, and composes the rest:

| latch to screen p50, 60 Hz, vsync on | buffers larger | the window's size |
|---|---|---|
| Direct3D 12, HD 630, windowed | 36.8 ms, composed | 20.2 ms, flipped |
| Direct3D 12, HD 630, fullscreen | 36.8 ms, composed | 20.2 ms, flipped |
| Vulkan through DXGI, HD 630 | 36.7 ms, composed | 20.1 ms, flipped |
| Direct3D 12, 1050 Ti | 36.3 ms, composed | 19.8 ms, flipped |
| Direct3D 12, HD 530, fullscreen | 36.8 ms, composed | 20.3 ms, flipped |

So a window at rest gets buffers its own size (`app_buffer_at_rest()`):
half a second after its size last changed the loop rebuilds once, and a
drag still finds room in larger ones. Fullscreen had them larger for a
second reason: the output a window has is the work area on Win32, which a
fullscreen window is taller than, and it was rounded past itself; and a
window made fullscreen before Windows has said what the output is, as the
bench's is, had them rounded with no output at all. Every way here is 19.8
to 20.3 ms now, windowed and fullscreen, but one: Direct3D 12 in a window
on aspire's HD 530, composed with buffers the window's size, 36.9 ms,
where a D3D11 swapchain there is flipped onto a plane and Direct3D 12
fullscreen is flipped without one. It is not the format nor the buffer
count: B8G8R8A8 in R8G8B8A8's place and two buffers in three's, alone and
together and with buffers the window's size, were composed too. It is the
driver's model: the HD 530's is WDDM 2.1 on 31.0.101.2111 and on .2115,
the last build Intel lists for the 6th generation (installed on aspire,
2026-10-30: composed still), where schenker's HD 630 is WDDM 3.1 and flips
Direct3D 12 in a window. Nothing of the library's swapchain decides it.
Nothing was hybrid, the copy between adapters PresentMon looks for.

**So where DWM composes it, Direct3D 12 presents through a D3D11
swapchain** (`present_d3d11.c`): its frames drawn into images shared with
a Direct3D 11 device on the adapter, copied into that device's swapchain
and presented there, the two queues ordered by two shared fences that the
GPU waits on and neither thread. Nothing tells before the first frames
whether a window will be composed -- a Direct3D 12 swapchain has no
`IDXGISwapChainMedia` to ask -- so the window is measured: at rest, with
buffers its size and vsync on, the frames between a present and the one
on screen (`GetLastPresentCount()` less the statistics' `PresentCount`)
over 60 presents (`via11_measure()`). A flip has one; the HD 530's
composed window read three or more on 478 of 480, the 960M's flipped one
one on 597 of 600. Fifty-four of sixty late, the swapchain is made again
from the Direct3D 11 device, and the window is measured there: flipped,
it stays; composed too -- another window over it, say -- it goes back.
Either way it is measured again at its next size, where a window that was
covered is likely clear: schenker's round, whose windows flip, read
composed on both ways at 1885x826 pixels, the window's bottom under the
taskbar, and went back. Never on a software adapter, which has no plane,
nor under wine, whose statistics are estimated. The HD 530 in a window, 36.9
ms from latch to screen, is 20.2 through it; the 960M stays on its own,
19.8. The cost is where it switches: a second device, a copy a frame, and
a quarter of the HD 530's uncapped rate (1311 to 988 fps) -- not paid
there, since vsync off is not measured. `FNL_D3D12_DXGI11=1` or `0`
decides it for a window, and two fault sites force a window read as
composed (`d3d12-composed`: through Direct3D 11, then back) and a
Direct3D 11 swapchain refused (`d3d11-present-refused`). On
macOS, the pointer moved through the measure: 70.7 ms from the library's
read to the screen and 77.6 from the input.

The record joins the stamps on the frame's index: the
display's own cadence (`shown_interval_ms`), a frame's age on the screen
from its latch (`latch_to_shown_ms`), and an input's from when the library
read it (`input.read_to_shown_ms`) and from when it happened
(`input.to_shown_ms`, from `FnlEvent::input_age`). On schenker, the
1050 Ti, 60 Hz, pointer motion through the measure at 100 Hz:

| | frame on screen p50 | read to screen p50 / p95 |
|---|---|---|
| KMS, Vulkan copy path | 6.5 ms | 13.0 / 20.5 ms |
| KMS, OpenGL bridge | 6.0 ms | 11.5 / 16.8 ms |
| weston, Vulkan | 13.7 ms | 18.7 / 25.7 ms |
| weston, OpenGL | 14.1 ms | 20.2 / 26.1 ms |

And X11, Intel's GPU (2026-10-29): schenker's Xorg, shown interval p50
16.661 ms, p99 16.674, latch to screen 21.4 ms with OpenGL and 53.6 with
Vulkan; this desk's XWayland, Vulkan, 16.667 and 54.0. OpenGL's swap
holds one frame ahead of the screen and Mesa's X11 Vulkan WSI three, the
pairing being the same code for both. The hold's jitter does not reach
Xorg's screen under OpenGL: the shown frames are 16.646 to 16.675 ms
apart.

**So on X11 a frame is held until the one before it is on the screen.**
Ahead of its latch, until the window server has answered for the last
frame presented -- the Present completion the backend pairs with it,
handed to core (`app_frame_answered()`) and waited on there
(`display_hold()`, where `FnlBackendVtbl::display_waits`: X11's alone) --
so it is latched, drawn and presented in the refresh that follows. Latch
to screen p50, every figure at 60 fps (2026-10-29): schenker's Xorg,
Intel's GPU, 53.7 ms as it was and 16.4 so; OpenGL there 21.4 and 16.4;
NVIDIA's own driver 36.3 and 16.5; this desk's XWayland 54.0 and 16.5. Held
first by `VK_KHR_present_wait` on the last present, a frame or two
deep -- 33.1 ms with two let wait, 16.5 with one, and fullscreen with MSAA
at 3840x2160 on the RX 590 16.5 -- and left: asked every frame, Mesa's X11
WSI stalled the loop under ThreadSanitizer, the window lost in nine
rounds of ten, three of three passing with the hold off; and NVIDIA's
driver offers no such wait. The server's own answer is any renderer's,
and three rounds of three pass on it. The wait is bounded at a period for
each frame still owed and half a period more, to a tenth of a second: at
a period and a half, XWayland's hold began with three frames waiting,
each wait ran out before the last was shown, and the queue was never
drained, 47 ms; and a wait that runs out stops the hold until an answer
comes, so a server that answers nothing costs one. One frame let wait
leaves a frame no overlap with the one before, so one whose work outruns
the refresh would miss every other where a frame more would have kept
the rate; two waits in a row over `LATE_MISS_FACTOR` of the period are
taken for that, and two frames are let wait for two seconds, the gap
doubling after each try that misses again, to 64. Shown keeping the rate
on schenker's Xorg (2026-10-30, the HD 630, Vulkan, fullscreen with MSAA
and the scene moving, 6.5 ms of GPU a frame) under a fixed late-latch
hold of 12 ms, so 18.5 ms from latch to the frame done: 59.6 fps, every
shown frame 16.661 ms apart (p99 16.676), 21.2 ms from latch to screen --
two frames in flight, where one would have made every other refresh; the
1 % low of 34 is the tries at one frame, each a refresh missed. At a
16 ms hold the latch itself reaches into the next refresh and frames miss
(shown p99 33.3 ms). A late-latch hold of 14 ms fixed came to 2.3 ms from
latch to screen at 60 fps on a light frame: the hold is waiting, not
work. The period sample counts the wait
(`App::display_block`), or the period would read short with the acquire
no longer blocking. Not while rebuilds keep arriving (`PACE_HOLDOVER`): a
drag is held to the size handshake already, and the frame between two
sizes waiting for the one before cost this desk's XWayland its border
row, 44 fps where the shown waiter had made it 59; skipped there, 60 --
figures of `pace.py`'s old count, which averaged lines picked by position;
counted over the press (`frames_between()`, 2026-10-31) the row reads 52
to 57.

From the input itself, weston and Vulkan again (2026-10-29): 24.5 / 30.1
ms, against 19.0 / 24.5 from the read in the same run -- 5.5 ms of
weston's delivery, which the read could not see.

**An event says how old its input is.** `FnlEvent::input_age` is how long
before the event's `time` the oldest input it carries happened: the
device's own stamp where the library shares its clock (*Input latency*),
0 where it does not, and 0 for what no hand did. An age and not a time,
and a float: it sits in the four bytes of padding after `type` on every
64-bit target, so `FnlEvent` keeps its size, which is the contract with a
caller's storage -- a double after the union would have grown it to 56
and wanted the minor bumped. An age is under `INPUT_AGE_MAX`, a second,
where a float's step is about a tenth of a microsecond.

weston's compositor frame is the refresh it adds. The stamps also say what
the callbacks' intervals smooth: Vulkan on weston misses a vblank now and
then (shown interval p99 33.3 ms) where OpenGL there does not (16.7 ms),
and every stamp on both was at a vblank.

**The tools around the record** are `tools/bench/`: `schema.json`, the
record's definition, which `check.py` enforces with a validator of the
dozen keywords it uses (no dependency, one definition); and `report.py`,
which reads a directory of records into a table a row a series, mean and
standard deviation over its complete runs, a run of one marked an
indication, and with `--html` a page of the same with its charts drawn
as SVG by the script, nothing loaded. `tools/drive/bench.py` runs the mode
in an environment as a round starts a program and keeps what comes back
-- on a guest, a laptop or the Mac through the round's own `start()`,
whose agent relays stderr home -- writing the tree's commit into the
record (`run.commit`, `run.dirty`), which the program cannot know. Its
`--hands` move the pointer round a small circle over the window through
each measure, found by the bench's two flushed lines around it (`bench:
measuring`, `bench: measured`; Windows' C runtime buffers a stderr that
is a pipe, and the line came home after the measure): uinput on weston
and KMS, XTEST on the bare Xorg, the agents' motion on Windows and the
Mac, with no hands on this desk, whose portal reaches whatever is
focused. `--sizes` runs an environment once a presented area -- asked
for in points and, on KMS, as a display mode, its window being its
output -- and `report.py --fit` fits a frame's cost against the area it
drew: the fixed milliseconds at no pixels and the milliseconds a
megapixel adds, which one size cannot separate. A key added after the
first records is never required, which is
the schema's additive rule applied to the past: the records phase 1 made
pass the check that phase 4 wrote.

**The panel is the mouse's through `fnl_claim()`** (*Replacing the
titlebar*): it claims its rectangle every frame, so a press on it is the
panel's and the scene around it still captures on a click. It keeps the
height of its longest page, so the buttons stay put as the page changes.
Every control has a key as well (Tab, the arrows, Enter, F5; R, B and Q on
the results), which is how a harness that cannot see works it. On Windows
(schenker, Direct3D 12) and macOS (Metal) a press on the panel is a
button event and no capture, and one on the scene beside it captures, as
on Linux: Win32's hit test answers the claim as client area. On macOS
the first click into a window that is not active, another program
frontmost, reaches the panel as well (`acceptsFirstMouse`, true outside
the content zone), and on the scene only activates the window.

First runs, on schenker booted from its stick (i7-7700HQ, GTX
1050 Ti and HD 630), 5 s measures, the testbed's scene: Xorg with no
window manager, Vulkan on the 1050 Ti, 60.0 fps, intervals p50 16.67 ms and
p99 17.0 ms; weston the same, p99 16.9 ms; KMS on the HD 630, p99 16.9 ms;
Xorg with vsync off 1436 fps on `IMMEDIATE`, intervals p50 0.69 ms; KMS with
vsync off stays on `FIFO`, which the record notes. The first run named a
renderer under a GPU ranking and found the ranking moving the window to
another renderer's entry (*Ranked by measured frame rate*: the name now
narrows the move).

## Source layout

```
include/fenestal/fenestal.h    the installed header: the whole portable surface
include/fenestal/fnl_vulkan.h  the Vulkan interop: what a caller records into
include/fenestal/fnl_gl.h      the OpenGL interop, the same for GL
include/fenestal/fnl_d3d12.h   the Direct3D 12 interop
include/fenestal/fnl_metal.h   the Metal interop

src/core/      every platform's. Names no window server and no graphics API,
               which tools/check-layering.py checks rather than asserts, with
               no file excused: each backend's and each renderer's state is
               behind an opaque pointer of App's (backend_state,
               renderer_state).
  app.h          private state + the interface between translation units
  compiler.h     what GCC, Clang and MSVC disagree about
  thread.h       mutex, condvar with a bounded wait, one thread, the clock
  backend.h      the backend seam: what the core asks of a window server,
                 and the entries a backend with nothing to do there may name
  renderer.h     the renderer seam: what the core asks of a graphics API
  app.c          library lifetime: fnl_init/run/shutdown, the key bindings,
                 and what every backend publishes the same way -- size, focus,
                 output, the held keys, the captured pointer's reticle
  error.c        the two error tiers: recorded failures and the log sink
  frame.c        the frame loop: pacing, late latching, the off-screen skip
  events.c       the event queue, and the once-a-frame diff that posts the
                 library's own notifications
  echo.c         the event echo, inside fnl_poll_event(), for a harness
  draw.c         what the library draws for itself, and the published
                 window measurements
  decor.c        the client titlebar's layout, its hit test, and what a
                 left press, a drag and a release do in it
  controls.c     the titlebar's three checkboxes and its output, GPU and size
                 drop-downs
  overlay.c      the three primitives, appended, and the palette
  text_measure.c UTF-8, the glyph table, the width of a string
  api_overlay.c  the public drawing calls, over the appenders
  api_state.c    the public knobs and the diagnostics
  metrics.c      the metrics panel, and the process's CPU, RAM and GPU
                 memory, sampled while it is shown
  rank.c         the GPU list ranked by measured frame rate, each entry
                 measured on each display and kept
  parts.c        what a frame's two cost figures are made of
                 (FNL_COST_PARTS=1)
  drivers.c      a GPU moved to another kernel driver with the window open
  fault.c        fault injection: the named sites a Debug tree forces
  dynlib.h       a runtime opened at run time rather than linked

src/backend/evdev_keys.h   evdev code to FnlKey: the table Wayland, X11 and
                           KMS share
src/backend/keymap.c       the layout, for text entry: keysyms to code points,
                           which level the modifiers select, and the dead
                           keys. Wayland, X11 and KMS share it too
src/backend/keymap_xkb.c   the reader for XKB's text keymaps, and the keysym
                           names they are written in; linked only where a
                           keymap arrives as text, which is Wayland and KMS,
                           whose files it follows the includes of
src/backend/keymap_rules.c the console's layout by name: xkeyboard-config's
                           rules, read for the files that make it. KMS alone
src/backend/keymap.h       their interface, which those three backends include
src/backend/keysym_table.h   keysyms and the code points they stand for, and
                             their names. Generated, and the one generated
                             file that is tracked; keysym_notice.txt is what
                             it travels under
src/backend/loop_linux.h   the wakeup eventfd and the epoll set, which the
                           three Linux backends share
src/backend/clip_linux.h   a paste arriving from another program: what the
                           Wayland and X11 backends share of it
src/backend/wayland/   registry, xdg-shell, keyboard and pointer
src/backend/x11/       ICCCM/EWMH, the pointer and the event loop
  x11_ext.c      the hand-encoded RandR and XKB requests, the monitor list
                 and the work area
  x11_keys.c     which key an event names, and what it types
  x11_clipboard.c  the CLIPBOARD selection, INCR included
src/backend/kms/       no window server: one output of the DRM node taken
                       whole, and the kernel's input devices read directly
  kms_driver.c   an NVIDIA GPU moved between nouveau and NVIDIA's module
src/backend/win32/     window class, message loop, raw input, per-monitor DPI
src/backend/cocoa/     the borderless window, its view and layer, the run
                       loop and its wakeup, text through NSTextInputClient;
                       Objective-C (DESIGN *macOS*)
                       Each has a <name>_state.h holding the handles no other
                       window server may see; only the backend's own files
                       and its bridges include it (surface_*.c, egl_*.c,
                       and on Win32 Vulkan's present_dxgi.c)

src/renderer/vulkan/
  vk_state.h     the renderer's state, RendererState, behind App::renderer_state
  vk_internal.h  what the renderer's files share: prototypes, the push-constant
                 structs and the asserts that pin them
  device.c       instance, device choice, loss recovery, the seam's table and
                 the public Vulkan interop
  swapchain.c    the swapchain, what exists once per image of it, the
                 window's pass and depth, and retiring it all across a rebuild
  scene_target.c the scene's offscreen target: its passes, its images, the
                 fetch back onto the window and the MLAA filter
  pipeline.c     the persisted pipeline cache, the one pipeline shape and the
                 one render-pass shape, memory types, the instance buffers
  frame.c        recording, submit and present
  text.c         the atlas decode and upload, and the glyph pipeline
  overlay.c      the rectangle and stroke pipelines, and the flush
  surface_*.c    the bridges: one per (backend, renderer) pair
  vk_load.c      the loader, opened at run time, filling vk_fns.h's pointers
  present_dxgi.c Win32: the frames presented through a DXGI swapchain

src/renderer/gl/       OpenGL, 4.5 core on Linux and 4.1 on macOS: device.c
                       (the bridge's display and context, the seam's table),
                       target.c, frame.c, overlay.c, loader.c (the entry
                       points, into one table), gl_compat.c (the 4.1
                       profile's stand-ins), memory.c (the process's GPU
                       memory); the bridges, egl.c with an egl_<backend>.c
                       each and cgl.m on macOS; egl_dyn.c, libEGL opened at
                       run time

src/renderer/d3d12/    Direct3D 12: device.c (the factory, adapter, device and
                       the seam), target.c (the swapchain's buffers and the
                       scene's targets), overlay.c, frame.c, present_d3d11.c
                       (the frames through a Direct3D 11 swapchain, where DWM
                       composes Direct3D 12's own) and surface_win32.c, the
                       bridge

src/renderer/metal/    Metal, in Objective-C: device.m (the device and the
                       seam), target.m (the drawables' size and the scene's
                       images), overlay.m (pipelines, atlas, arenas),
                       frame.m, and surface_cocoa.m, the bridge

testbed/
  main.c         the program, as a client of the library
  testbed.h      what the testbed's files share; it names no graphics API
  scene_<api>.*  the test pattern, as each renderer's client records it
  bench*.c       the benchmark mode (--bench, *Benchmarking*)
  overlay.c      the keybindings panel, the status lines and the crosshair
  titlebar_custom.c    a whole replacement titlebar, drawn by the testbed
                       (--custom-titlebar)
  shaders/       GLSL the testbed owns: the test pattern and the star

examples/        the API reference's snippets, each a complete program
demos/<name>/    a program to copy: main.c, a scene file per renderer, its
                 shaders and a CMakeLists.txt that also configures on its
                 own (demos/README.md)

tools/mkfont.c        bakes the glyph atlases at build time (FreeType is here)
tools/verifyfont.c    checks the bake against FreeType, texel by texel
tools/verifykeymap.c  checks the layout code against a keymap, and its parser
                      against that keymap damaged; run by every native build
tools/font_charset.h  which code points a size is baked for, shared by both
tools/font_codec.h    the blob's container format: encoder and decoder
tools/font_raster.h   FreeType in, coverage out: the one rasteriser the bake
                      and its check both call
tools/check-layering.py  the layering check
tools/drive/          the harnesses that drive a built window with synthetic
                      input; artifacts land in build/drive/
tools/drive/stress.py    the correctness round, in every environment
tools/drive/environment.py  what a round may ask of a window, per
                            environment
tools/drive/testbed.py   the process under test and the events it echoes
tools/drive/wayland_driver.py  the RemoteDesktop portal session and the
                               aiming: the one way in, for every
                               environment on this desktop
tools/drive/x11_driver.py      what an X server can be asked about a window;
                               it drives nothing
tools/drive/options_wayland.sh  the Wayland build under the checkers, across
                                the command-line options
tools/drive/dev.c     fenestal-dev, built into every tree: one program
                      with a mode each, from the two files below
tools/drive/pace.c    its pace mode, the one that drives the *library*
                      instead: a caller asking for its own sizes
tools/drive/lifecycle.c  its lifecycle mode: the API called wrongly, then
                      many lifetimes in one process, what each holds
tools/probe/          what a machine was asked, kept as sources and built
                      by hand, each one file; a Windows one by
                      `x86_64-w64-mingw32-gcc -O2 f.c -lgdi32 -luser32`
tools/probe/waittest.c  how late a timed wait returns, by kind of wait
tools/probe/vmcost.c    what a clock read and a thread hand-off cost
tools/probe/cpubench.c  raw float throughput by thread count
tools/probe/dibtest.c   what a software present costs: the copy and the blit
tools/probe/sampler.c   a sampling profiler that needs no elevation
tools/probe/stacks.c    every thread's call stack, for a program that stopped
tools/probe/wherewin.c  where Windows says a window and the pointer are
tools/probe/modes.c     a desktop's displays, adapters and rates; sets one
tools/probe/scale.c     a display's scale, as Settings has it; sets it
tools/probe/nap.c       sleep, a timer to wake by, and input once awake
tools/probe/d3d12ts.c   where a Direct3D 12 list's first timestamp lands
                        against the work submitted ahead of it
tools/probe/keymapdump.c  the keymap for a layout, from the files or from a
                          compiled keymap's text; xkbdiff.py runs every
                          layout through both and prints what differs
tools/probe/native_nap.py  runs it on the laptop's desktop and watches the
                           machine go and come back
tools/probe/guest_run.py  runs one of them, or anything, in the guest
tools/probe/guest_pace.py  pace.py's sweep put to the guest, which pace.py
                           leaves out; frames only, and no word on pacing
tools/drive/pace.py   runs it in every environment and tabulates the three
tools/mkdist.py       the source distribution: the build, docs/ and tools/
                      whole and the top-level .md, comments stripped, and
                      fenestal-commented.zip, the same with them;
                      --worktree (`./build.sh dist`) packs the working
                      tree as it stands, untracked files included
tools/mkkeysyms.py    regenerates src/backend/keysym_table.h from X11's
                      keysymdef.h; run by hand, a few times a decade
shaders/       GLSL the library owns: the three overlay pipelines, the
               covering triangle, the fetch and the MLAA filter
protocols/     vendored Wayland protocol XML
fonts/ubuntu/  vendored Ubuntu Sans + Ubuntu Sans Mono, with the licence
cmake/mingw-w64-x86_64.cmake  the Windows cross-compile toolchain
CMakeLists.txt the build of one tree
build.sh       the build as one command: every tree, or one target and variant
run_all.sh     the release testbed four ways at once, with the same options:
               Wayland and X11, and under wine Direct3D 12 and Vulkan; it
               builds nothing
fenestal.map   the shared object's exported surface, as a version script
tsan.supp      ThreadSanitizer suppressions, with the reasoning for each
lsan.supp      LeakSanitizer suppressions, the same
```

Includes are paths from `src/`: `#include "core/app.h"`, first in every `.c`.

### Two dispatch tables, not one

The backend and the renderer are separate tables. Vulkan runs on every
backend while a second graphics API would run on one of them; folded into one
table the cost is backends times renderers instead of backends plus
renderers.

What genuinely *is* a pair is the bridge — turning a native window into
something presentable. `create_surface` lives in the renderer table because
the object it produces belongs to the renderer and dies with it, and there is
one file per real pair, each defining one table (`VkBridge`):
`surface_wayland.c`, `surface_x11.c`, `surface_win32.c`, `surface_cocoa.c`,
and `surface_kms.c`, which does more than the others because a bare display
has no window to wrap (*KMS: no window server*). Its first entry is the list
of instance extensions the pair needs beside `VK_KHR_surface`: one required
for a window server, others asked for where offered, and five for a bare
display, none of them required. The renderer takes the table of the
backend that opened, since a Linux build has three (*One build per
operating system*).

What is deliberately *not* in the backend seam: no native handle, no "where is
the window on screen" (Wayland cannot say), no pointer warp (Wayland will not).
Each would be a call that some backend cannot answer.

Two entries are declared properties rather than inferred:
`FnlBackendVtbl::grab_ring`, because the transparent resize margin is a Wayland
answer to a Wayland problem, and `FnlBackendVtbl::invites`, because whether the
window server invites frames decides which late-latch signal the frame policy
reads. Every table states both; one left out would read as 0.

**Every entry is named, including the ones a backend has nothing to do in.**
`backend.h` declares shared `backend_noop*` entries and
`backend_frame_wait_shown()` — the frame wait for a window server that
invites nothing, answered from the backend's own `window_on_screen` — and a
backend puts those in its table by name, so the core calls every entry
without testing it and a table says in its own text which ones do nothing.

**When an entry can run is the seam's rule, not each backend's test.** Every
entry except `close()` and `wakeup()` runs only between a successful `open()`
and the start of `close()`, so a backend does not test `App::backend_state` or
its window there. Those two do: `close()` is also the failed-open path out of
`fnl_init()`, and `wakeup()` may arrive from a signal handler or another thread
at any moment. The frame's entries — `frame_wait`, `frame_request`,
`frame_cancel`, `window_on_screen`, `set_geometry`, `ack_configure`,
`apply_scale`, `present_pixels` and `flush` — are the render thread's, which
is joined before `close()` runs; the rest are the pump's, but for those whose
comment in `backend.h` names another thread (`buffer_granularity`,
`present_window_*`, `ack_release`, `host`, the `gpu_driver*` entries), and
`absent()`, asked before any `open()`.

**What every backend would otherwise write three times is the core's.**
`request_size` is handed the *window* size in points, the bar already added,
and is reached only through `app_request_size()`, which refuses while the
window is fullscreen or maximized: `fnl_request_size_pt()` promises that, and a
window server cannot be relied on to decline by itself. The titlebar's hit
test (`deco_hit()`), what a left press, a drag and a release do in it
(`deco_press()`, `deco_drag()`, `deco_release()`), which shape the pointer
takes (`deco_cursor()`), and the publishing of a size, a focus change and an
output (`app_publish_size()`, `app_publish_focus()`, `app_publish_output()`)
are one implementation each, in `src/core/`. A backend translates its window
server's events into those calls and its answers into requests.

### One build per operating system

One build per operating system -- `linux`, `win64`, `macos` -- holding
every backend and every renderer that system has, with the platform chosen
in `fnl_init()`. The first of the three steps below is built and driven: a
Linux build has Wayland, X11 and KMS, and the backend is chosen when the
program starts. The second -- every runtime opened at run time, none
linked -- and the third for Windows -- Direct3D 12 and Vulkan in one
build, the renderer chosen on the open window -- are built and driven
on this desktop, the bare machines, the guests and the Windows laptops
(TODO.md says what those still owe). The third, for Linux and macOS --
Vulkan and OpenGL, and Metal, Vulkan and OpenGL 4.1 -- is built and
driven too. What follows is that design, and the order it was built
in. One platform per build had a caller shipping for Linux
pick Wayland, X11 or KMS before it could know which one the user's machine
runs, and made eleven targets and forty-seven trees.

| system  | backends          | renderers                    |
|---|---|---|
| Linux   | Wayland, X11, KMS | Vulkan, OpenGL               |
| Windows | Win32             | Direct3D 12, Vulkan          |
| macOS   | Cocoa             | Metal, Vulkan (on MoltenVK), OpenGL 4.1 |

Only Linux has a choice of backend; on the other two the choice is the
renderer's alone. OpenGL on macOS is 4.1, through CGL, its frames copied
into the Metal layer (*OpenGL on macOS*); Windows draws with Direct3D 12,
on WARP where there is no driver, and Vulkan.

**What fitted already.** Every call to a backend or a renderer goes through
its table, so choosing one at run time changes nothing per frame.
`FnlRenderer` has `FNL_RENDERER_DEFAULT`, `fnl_renderer()` and
`fnl_backend_name()` report the answer, and `FnlCallbacks` names no graphics
API -- a scene asks its interop header for what to record into -- so a
caller with a scene per renderer will switch on `fnl_renderer()` inside
`on_record` and need no callback of a new shape.

**The backends, as built.** `backend.h`'s `FNL_BACKENDS(X)` is the list
of what the build has, in the order tried: Wayland, X11, KMS, each there by
a `-DFNL_HAVE_<NAME>` that CMake passes with its sources. `FNL_BACKEND` at
configure is `linux` for all three, the default, or one name for a build of
that one alone. `backend_choose_and_open()` (`app.c`) walks the list:

- **`absent()` first**, from the environment alone: Wayland where
  `WAYLAND_DISPLAY` or `WAYLAND_SOCKET` is set, X11 where `DISPLAY` is, KMS
  where none of the three is. Wayland is not tried on the strength of
  `wl_display_connect(NULL)`'s own guess, `wayland-0`, which reaches the
  desktop's compositor from an ssh session or a console; and KMS is not
  tried in a session that has a window server, even one that refused,
  since the display is that server's and taking the console from under it
  is what nobody asked for.
- **Then `open()`.** `FNL_ERR_NO_DISPLAY` is the one refusal stepped down
  from -- there is no window server here to speak to -- and every backend
  already returned it for that and before its window existed: no
  compositor, a compositor without `xdg_wm_base`, no X connection, a DRM
  node in another program's hands. `close()` is `open()`'s failure path,
  so what the refused one built is gone before the next starts. Any other
  failure ends `fnl_init()`, since the next backend would meet it too.
- **One line says what happened**: `backend: X11; passed over Wayland: no
  WAYLAND_DISPLAY`. `FNL_BACKEND=x11` in the environment names one
  instead -- `backend: X11, named by FNL_BACKEND` -- and a named backend
  that fails is a failed start, never a step down: a forced run that quietly
  ran another backend is a figure that lies. The harness names one for
  every round. `backend-no-display` is the fault site that makes a chosen
  backend refuse, so that the step down runs on a desktop that has both.

The bridges were chosen by file name in `CMakeLists.txt`, and each pair
defined the same symbols -- `create_surface` and the instance extensions in
each `surface_<backend>.c`, eight `egl_native_*` functions in each
`egl_<backend>.c` -- one definition per build. Each file now defines one
table, `VkBridge` and `EglBridge`, of static functions, and its renderer
finds the one whose `backend` is `FnlBackendVtbl::id`, from a list made from
`FNL_BACKENDS(X)` as the core's is, so the two cannot disagree. Still one
file per real pair, and still the only files that see both halves.

Every backend's state is still `struct BackendState`, a definition in
each `<p>_state.h`, and a Linux build now links three. No translation unit
sees two, and `App::backend_state` is a pointer whose type is only completed
inside one backend's files; gcc's LTO, the one pass that sees them all at
once, compares pointer members without what they point to, and the sweep's
23 trees built without a warning from it or anything else. A tag of each
backend's own is the change to make if a checker ever objects.

**Measured** (2026-10-22). Every Linux environment, each named its
backend, passed its round with none failed: the four of this desktop at
`release`, `asan`, `tsan` and `gc` (58 to 80 checks), the six of the bare
machine at `release` (43 to 66), and the wine environments' three trees
beside them, whose Win32 changed by its table's three entries. Unforced,
the choice is the one `absent()` predicts:

| session | opened, and the line |
|---|---|
| this desktop, both variables | Wayland |
| this desktop, `WAYLAND_DISPLAY` unset | X11; passed over Wayland: no WAYLAND_DISPLAY |
| the bare machine, weston's socket alone | Wayland |
| the bare machine, Xorg's `DISPLAY` alone | X11; passed over Wayland |
| the bare machine, neither | KMS; passed over Wayland and X11 |
| this desktop, `FNL_FAULT=backend-no-display@1` | X11; passed over Wayland: Wayland refused (FNL_FAULT) |

`FNL_BACKEND=kms` on this desktop is refused at `open()` (the DRM node is
the compositor's) and ends `fnl_init()`, as a named backend should; a
name the build has not got is refused with the list it has.

**The link line.** Every runtime on the link line would make the program
fail to *load* where one is missing, before any fall-through could run: a
Windows guest with no Vulkan driver has no `vulkan-1.dll`, and a console
machine need have no `libwayland-client`. So what is not part of the system
is opened at run time, and a consumer links the system's own libraries and
nothing else:

- **Linux**: `m`, `pthread` and `CMAKE_DL_LIBS` (nothing from glibc 2.34,
  where `dlopen` is libc's; `libdl` before it). `libvulkan.so.1`,
  `libEGL.so.1`, `libwayland-client.so.0`, `libwayland-egl.so.1` and
  `libxcb.so.1` by `dlopen`.
- **Windows**: `user32`, `gdi32`, `d3d12`, `dxgi` and `dxguid`, all part
  of the system from Windows 10. `vulkan-1.dll` by
  `LoadLibraryEx`, from the program's directory or System32 and never the
  current one.
- **macOS**: AppKit, QuartzCore and Metal, and OpenGL and IOSurface where
  the tree has OpenGL; MoltenVK by `dlopen` from beside
  the program (`@executable_path`), where `build.sh` puts it, or a
  loader the system has.

**The runtimes, as built.** One shim, `src/core/dynlib.h`, opens a library from
a list of names and finds its symbols; a library once opened is never closed
(but for EGL where a GPU may move to another kernel driver, `egl_dyn.c`), since
Mesa beneath every one of these leaves thread-locals and exit handlers that
outlive an unload, and each loader opens once per process and keeps its answer,
failure included. Every runtime is reached the same way: its header is included
first, with its prototypes; an X-macro list names every entry point the code
calls; each becomes a pointer of the prototype's own type (`__typeof__`), and a
`#define` puts the pointer in the name's place, so not one call site changed and
the compiler checks every call against the real prototype. A function missed
from a list is a call straight to the library, which then fails to link in
`fenestal-dev`, a program that links the library and nothing else, as every
program the build makes does.

- **Vulkan** (`vk_fns.h`, `vk_load.c`): `vkGetInstanceProcAddr` from the
  loader, the global entries through it, and every instance and device
  entry through it again once the instance exists -- the loader's
  trampolines, which is one indirection more per call than a device's own
  table, for a render loop whose calls are counted in tens a frame. A
  window server's own entries (`vkCreateWaylandSurfaceKHR` and the rest)
  are asked for in its bridge at the call, since their prototypes need that
  server's headers. No loader, or no driver beneath it, is
  `FNL_ERR_NO_DEVICE`, which the renderer choice steps past.
- **EGL** (`egl_dyn.h`, `egl_dyn.c`): seventeen entries, opened as the GL
  renderer's bridge first opens; GL itself was always reached through
  `eglGetProcAddress`.
- **xcb** (`xcb_dyn.h`, `xcb_dyn.c`): seventy, core xcb's and the extension
  plumbing `x11_ext.c` encodes its requests through; opened first thing in
  the backend's `open()`, so a machine without it is `FNL_ERR_NO_DISPLAY`
  before anything exists, and the backend choice steps past it.
- **Wayland** (`wl_dyn.h`, `wl_dyn.c`): eighteen, which with the inline
  requests of the system's protocol header and the generated ones is all
  libwayland-client is asked. What `wayland-scanner` writes also points at
  the core protocol's interface objects (`wl_surface_interface` and the
  rest) by name, from tables a static initialiser fills, which a library not
  yet opened cannot answer; so the build generates the core protocol's
  code itself, from the `wayland.xml` the installed headers were made from,
  and the library has hidden copies of its own. The version has to be the
  headers': their inline requests index those tables by opcode.
  libwayland compares interfaces by name, so its copies and these agree.
  libwayland-egl, the GL bridge's, is opened in `egl_wayland.c` when the
  bridge first wants a window.

The invariant's "plus the runtime of the platform it chose" becomes "plus
the system's own libraries"; a library something needs is still a
subprocess. A caller's Vulkan scene need not link the loader for its
`vkCmd*` either: `FnlVulkanGpu::get_instance_proc_addr`, at the struct's
end, is the loader the library opened (MoltenVK on a Mac), and the
examples' scenes ask it, with the instance, for every entry they call,
each a pointer of the function's own name under `VK_NO_PROTOTYPES`, so
that a call not asked for is a compile error and never an import. Linked,
the testbed and `fenestal-minimal` did not start at all on a Windows
machine with no `vulkan-1.dll`, where the library would have stepped past
Vulkan to Direct3D 12. The device's entries are asked of the instance and
come back as the loader's trampolines, as the library's own do; a scene
heavy enough for the dispatch to show would ask `vkGetDeviceProcAddr`
instead. The cross trees still synthesise an import library and a
MoltenVK stub, for a caller that links the loader anyway.

Measured, 2026-10-28 at `96cd5b6`: the win64 programs import Windows' own
DLLs alone, the Linux ones libc and libm, the macOS ones the system's
frameworks. In the win10 guest, the testbed and `fenestal-minimal` started
from a folder beside a `vulkan-1.dll` that is no DLL -- the program's
folder is searched first, by the process and by `LoadLibraryExA`'s
default directories alike, so the system's loader is shadowed for both
and the guest's install is left alone (renaming it takes an
administrator, which guest control is not). Each said `no Vulkan loader:
vulkan-1.dll: not found (error 193)` once, drew on Direct3D 12 on WARP and
ran until stopped 40 s later; `vulkaninfo.exe` beside the same file found
no loader, which shows the shadow holds. A program started so writes its
GPU ranking without Vulkan's entries, so those files were deleted after.
The stress rounds the same day passed with the scenes on the pointers:
`x11-vulkan` (101 checks), `win64-vulkan` under wine (85) and
`win64-vulkan-vm` on win10 and win11 (87 each), each moving the window to
another GPU and back -- a new instance every time, the entries asked
again.

**Measured with each one missing**, 2026-10-28 on this desktop's GNOME:
`fenestal-pace`, which links libc alone, run under bubblewrap with the
library directory overlaid and the runtime deleted in the overlay, so
`dlopen` meets a missing file as a machine without it would. No Vulkan
loader: Wayland, OpenGL, `passed over Vulkan: no Vulkan loader:
libvulkan.so.1: ...`. No `libEGL`: Vulkan, OpenGL listing no GPU. Neither:
`fnl_init()` fails, one line naming both. No `libwayland-client`: X11,
`passed over Wayland`. No `libwayland-egl` with OpenGL named, or no
`libxcb` with X11 named: the named one fails, one line naming the
library. And no `libwayland-client` or no `libxcb` with neither named:
the backend opens, and then no renderer draws -- Mesa's Vulkan drivers and
its EGL link both libraries themselves, so a machine missing either has
no Mesa that loads, and the line says `no Vulkan driver would load`,
not that none is installed. Each refusal is said once: the loader's,
asked again by the renderer after the GPU listing, and the driver's,
asked by both, are recorded quietly the second time (`fnl_fail_quiet()`);
and a list of names reports the first's reason, the runtime's own name,
not a fallback's (`fnl_lib_open()`).

**The choice is an order, not a measurement.** A benchmark at startup costs
the startup, answers differently from run to run, and leaves the harness and
the ledger unable to say what ran. A fixed order, a step down on failure,
and one correction:

- **Backend, on Linux**: Wayland, then X11, then KMS, as built above.
  Wayland first because on a Wayland desktop X11 is XWayland, a second
  server between the window and the compositor. A backend whose `open()`
  refuses has shown nothing, so the step down is free.
- **Renderer, on Windows**: Direct3D 12, then Vulkan. Direct3D 12 first
  because every Windows 10 has it, on WARP where there is no driver; Vulkan
  wants a vendor's driver, which the guests do not have.
- **Renderer, on macOS**: Metal, then Vulkan, then OpenGL 4.1: one layer
  fewer, and MoltenVK may not be beside the program.
- **Renderer, on Linux**: Vulkan, then OpenGL.
- **The correction**: a renderer whose best device is software -- lavapipe,
  llvmpipe, WARP -- ranks after one whose best device is hardware, which is
  Vulkan with only lavapipe on a machine with a hardware GL driver. Vulkan
  answers it from the ranking the startup inventory already makes
  (`device_type_rank()`, `phys_software`), OpenGL from its renderer string.

As much as can be is decided before `open()`. An instance and a device that
could present answer most of it -- the loader, a driver, a device -- and Vulkan
answers presentation without a surface
(`vkGetPhysicalDevice*PresentationSupportKHR`, for Wayland, XCB and Win32). The
X11 window is made with the root visual and EGL's configuration follows it, so
that backend's window does not wait on the choice either. A failure after the
window exists, in `create_surface` or `create_device_objects`, steps down to the
next renderer on the same window, the backend kept: closing it and opening
another shows a window that vanishes. Device-loss recovery stays on the renderer
chosen; changing graphics API mid-run would hand the caller a different interop
header between one `on_gpu_created` and the next. Each step down is a fault site
of its own, so that it runs in a Debug tree.

**The caller's half.** `FnlInitDesc` grows `uint32_t renderers` at its end, a
mask of the renderers the caller has a scene for, and the order runs over those.
`renderer` keeps its meaning: that one, or refused. With neither, a build of one
renderer behaves as it does today and a build of several refuses with
`FNL_ERR_UNSUPPORTED`, naming the field: a caller that wrote one scene must not
be handed another API, the rule `fnl_init()` already keeps. The caller's own
order was considered and left out: a caller that prefers one names it in
`renderer`, and refused, calls `fnl_init()` again with the mask. Both examples
have a scene per renderer and declare every one their system has. The ABI
baseline, a `CHANGELOG.md` line and a snippet go with it.

**The renderers, as built.** `renderer.h`'s `FNL_RENDERERS(X)` is the list
in the order above, each renderer there by a `-DFNL_HAVE_RENDERER_<NAME>`;
`FNL_RENDERER=all` at configure, the default, is every one the system has,
and one name builds that one alone. `fnl_init()` asks twice: before
`open()`, whether the caller may be handed any renderer of this build
(`renderer_check_choice()`), and once the window is open, which one
(`renderer_choose_and_build()`, `app.c`). `FNL_RENDERERS_ANY` is the mask
of a caller with no scene -- `fenestal-dev`'s two modes, the
snippets that draw only the overlay -- which any renderer will do for. Each
candidate is built whole on the open window -- instance, surface, device
-- or torn down as `app_teardown()` would, leaving no renderer behind, and
the next one starts; a failure for want of memory ends the start, since the
next would meet it too. One line says what happened: `renderer: Vulkan;
passed over Direct3D 12: <why>`, or `renderer: Direct3D 12, named by
FNL_RENDERER`.
`renderer-refused` is the fault site that makes a candidate refuse.

What a build of several needed of the renderers themselves: the four
entries the core called by name -- the two overlay flushes, the scissor and
`text_end()` -- became table entries, since each renderer defined them;
each interop call refuses an App another renderer draws, every one being
exported; and `fnl_renderer()` answers the App's renderer, the build's only
where it has one.
**The correction is made after the first build, and only where it lands on
software** (`renderer_correct()`). A renderer whose GPU in use is a
software rasteriser, with no GPU named, lists the candidates after it with
`list_gpus` -- the same listing a move uses -- and gives way to the first
that lists hardware: torn down, the other built on the same window, one
line, `Vulkan would draw on software here (llvmpipe …), and OpenGL has
hardware (AMD Radeon RX 590 …)`. A machine whose first renderer found
hardware lists nothing at startup, which is every machine but that one.
Where the other will not build, the first is built again on its software
(`renderer-refused@2` forces it: `OpenGL has hardware here … and would not
draw …; Vulkan draws on software`). A named renderer or a named GPU is
never corrected. Seen on this desktop with Vulkan shown only lavapipe
(`VK_DRIVER_FILES`): the window opened on OpenGL's Radeon. On Windows
Direct3D 12 comes first, whose WARP is the software device of a machine
with no driver, and a guest has no other: Vulkan there lists lavapipe,
and nothing changes.

**Measured** (2026-10-22, the second and third steps). The seventeen trees
built without a warning once one test was put in the form clang's
`-Wunreachable-code` reads as a constant on purpose. The shared object and
`fenestal-dev` need libc and libm alone on Linux and Windows' own DLLs
alone on Windows (`d3d12`, `dxgi`, `GDI32`, `KERNEL32`, `msvcrt`,
`USER32`); the testbed adds the Vulkan loader, for its own scene. Every
round passed with none failed, each saying the backend and the renderer it
was named: the twelve bare environments on the aspire and on the thinkpad,
Linux's six and wine's six; this desktop's four and wine's four; and both
renderers in both guests, 72 checks each. Unforced under plain wine, with
no vkd3d-proton beside the program, the order steps down as built:
`renderer: Vulkan; passed over Direct3D 12: no adapter builds the
library's Direct3D 12 pipeline states, and there is no WARP` -- wine's own
vkd3d makes a device and no pipeline from DXIL, which the adapter list
asks of each adapter (`d3d_draws_on()`, *Moving to another renderer*).

And the third for Linux and macOS, with the Windows laptops beside it
(2026-10-22): every round passed with none failed, each saying its
backend and renderer -- this desktop's four, the six Linux environments on
the aspire and on the thinkpad, the Mac's two (Metal and Vulkan on
MoltenVK, 62 checks each), and on each laptop's Windows both renderers on
each of its two devices (the aspire's HD 530 and GTX 960M, 67 checks each;
schenker's HD 630 and GTX 1050 Ti, 68 each), each on the device it was
named. Unforced on the laptops the order took Direct3D 12, on the NVIDIA
GPU (*The startup inventory*).

Unforced elsewhere (2026-10-31, `1d987ac`). On the Mac the testbed opened
`renderer: Metal` on the HD 630 and listed MoltenVK and OpenGL after it,
in the order above. In both guests the first start opened Direct3D 12 on
WARP, the order's; the ranking then measured both entries there (win10
Direct3D 12 83.0 fps, Vulkan on lavapipe 79.0; win11 75.5 and 90.1), and
the next start opened on its first -- Direct3D 12 in win10, Vulkan in
win11. So with a ranking kept the order is the measured one, which on two
software devices a few frames apart may be either. `renderer-refused@1`
in the `win64 gc` tree stepped down in both (`renderer: Vulkan; passed
over Direct3D 12: ...`, and the other way in win11), and once the refused
renderer's GPUs were listed the window moved to its ranked first under
the live window.

A window takes the next renderer's surface after the first one's has
failed, not only after it drew (2026-10-31, `1d987ac`, the `gc` testbed on
schenker's HD 630, no renderer named): with `FNL_FAULT=framebuffers@1`
Vulkan's first build fails after its swapchain is made on the window, is
torn down, and OpenGL draws the same window at 60 fps -- `renderer:
OpenGL; passed over Vulkan: FNL_FAULT: create_framebuffers` -- on bare
Xorg, on weston (its screenshot the testbed whole) and on KMS, whose
scan-out showed the frame from 0.9 s, black before it; each with no
warning or error, and exit 0. On both laptops' Windows the same fault
fails Vulkan's build after its DXGI flip-model swapchain is made on the
HWND, and Direct3D 12 takes the window (`renderer: Direct3D 12; passed
over Vulkan: FNL_FAULT: create_framebuffers`), at the start and again
when the ranked move back to Vulkan fails the same way; once Vulkan's GPUs
are listed with the fault spent, the window moves back to Vulkan's own
swapchain under the live window. And the other way round,
`FNL_FAULT=d3d12-after-swapchain@1` with no renderer named: Direct3D
12's build fails with its flip-model swapchain on the window, and Vulkan
through DXGI takes the HWND and draws (`renderer: Vulkan; passed over
Direct3D 12: FNL_FAULT: after the swapchain`, aspire's HD 530 and GTX
960M); a later move to the 960M's Direct3D 12 lands on that window, a
dissolve with no black sample. On Cocoa, with
`FNL_FAULT=metal-after-layer@1` and no renderer named, Metal's build
fails with the window's `CAMetalLayer` given its device, and MoltenVK
takes the same NSWindow (`renderer: Vulkan; passed over Metal: FNL_FAULT:
after the layer`); with `framebuffers@1` beside it Vulkan fails too and
OpenGL 4.1 draws the window through its bridge (schenker's HD 630,
2026-11-01, the `macos gc` tree, both pictures the testbed whole).

**Forcing it.** `FNL_BACKEND=x11` and `FNL_RENDERER=gl` in the environment,
spelt as the configure options spell them, take that one or fail, with no
step down: a forced run that quietly ran something else is a figure that
lies. `FNL_GPU` stays the device's (*The startup inventory*). The inventory
says which platform it took and, a line each, why every one above it was
passed over.

**Builds and rounds.** The targets are `linux`, `win64` and `macos`, and
`linux-arm64` and `macos-arm64` for the other processors, with the variants
they take now: six of the native target, three of Windows and two of each
macOS one, thirteen trees where there were forty-seven, and the ARM Linux
one's four where its compiler is. The cross trees borrow `linux/release`'s
bake. A configure option keeps a build of one platform (`FNL_BACKEND`
taking one name as well as `linux`, as it does now, and `FNL_RENDERER` one
as well as all) for a consumer who wants the smaller library; the sweep
builds only the full ones. The twenty-seven environments stay: each names a
tree and a platform, forced by the two variables, and the ledger records
the platform the inventory reports rather than the tree's name. One check
joins the round, unforced: that each environment takes what the order above
says it should -- Wayland with Vulkan on this desktop, Win32 with Direct3D
12 on WARP in the guests.

**What every renderer costs in bytes** (2026-10-31, `c3b3aeb`, release):
little. The Linux shared object is 1.71 MB with three backends and two
renderers -- 441 KB of code, 1.09 MB read-only -- and the arm64 one 1.70;
the Windows DLL 1.58 MB and the macOS dylib 1.40. Of the Linux one, 955 KB
is the font (the atlas 926 KB, its cells 29), and the shaders in both
translations 40 KB: SPIR-V 29.6 KB, Vulkan's, and GL's GLSL 10.8 KB. On
Windows the shaders are SPIR-V's 29.6 KB again and Direct3D 12's DXIL 153
KB, a signed container being five times the SPIR-V it is made from; on
macOS SPIR-V for MoltenVK and Metal's MSL, 15 KB. So the second renderer
of a system costs some 11 KB of shader on Linux and some 150 on Windows,
and the code that draws with it; the font is half of every tree, whichever
renderers it has.

In this order, each step leaving every round passing:

1. **The Linux backends in one build**, the renderer still chosen at configure:
   the bridge tables, the backend order and `FNL_BACKEND` at run time, the
   runtimes linked as now. A caller sees nothing change; eleven targets became
   seven, `linux-vulkan` and `linux-gl` for the six, and forty-seven trees
   twenty-three. Built and driven.
2. **The runtimes loaded, not linked**, Linux first: Vulkan's table, EGL,
   xcb, then Wayland's redirect header. A build check joins the ABI's: the
   shared object's undefined symbols are the system's and nothing else
   (`--no-undefined`, at its link). Built and driven.
3. **Every renderer in one build per system**: the mask, the renderer order and
   its correction, the step down on the same window, `FNL_RENDERER`, and every
   scene of each example compiled in. Built and driven: `win64`, `linux` and
   `macos` for the seven targets of one renderer each, with an ABI baseline of
   the two Linux renderers' exports (`fenestal-0-all.syms`); the correction left
   out.

### Threading

The main thread owns the window server connection and all input; the render
thread owns everything the graphics API touches. They share only the block
guarded by `App::lock` plus a few atomics.

The main thread blocks in the backend's `pump()`; the render thread wakes it
through `wakeup()`. Traffic goes the other way too: the render thread requests
the frame callback and acks the configure just before the present, so both ride
on the commit the present performs.

On Wayland the epoll set must stay **level-triggered**:
`wl_display_read_events()` does one read rather than draining to `EAGAIN`, so
edge-triggered registration could leave buffered bytes arming no further edge.
The display is watched for room too, but only while the pump's flush has
found the socket full: level-triggered, `EPOLLOUT` on a socket with room
would end every wait at once, and without it a request the main thread
buffered -- a title, a size floor -- would wait for whatever woke it next.

On X11 "owns the connection" is literal: there are **two**, one per thread, and
a *reader* must not cross between them. See *Frame pacing and the off-screen
skip* for what a shared one costs.

The wakeup is an eventfd in an epoll set on the three Linux backends, and
closing those two descriptors turns on whether the caller took the process's
signals. `on_terminate()` reaches `app_wakeup()`, and a process-directed signal
lands on whichever thread the kernel picks — one that loaded the `App` pointer
before teardown cleared it could `write()` to a descriptor number since
recycled. So teardown puts the handlers back, clears the pointer and waits the
handlers out before the descriptors close; only where one is still running after
a second (stopped where it stands) are the two kept until process exit. Every
other caller has no such handler — `install_signal_handlers()` is the only thing
that ever stores that pointer — so its descriptors close with everything else.
Keeping them for every caller costs two descriptors per
`fnl_init`/`fnl_shutdown`, and a host embedding this — which opens and closes
windows for as long as it runs — is exactly the caller that would not hand a
library its signals.

Everything a `[any]`-thread public call answers about the window comes out of
one published block (`App::pub_*`), filled once a frame by the render thread
under the lock it already takes. What comes back describes the frame that was
last *built*, so a control hit-tested during a drag-resize is tested against
that frame's width — one frame of lag, and a set that is internally consistent.
Computing each answer from `App::extent`, `draw_margin`, `draw_scale120` and
`scene_extent` instead would be reading four render-thread fields from another
thread, which is undefined rather than merely stale — and a width and a height
could come from two different resizes.

The same rule covers the rest of the public readers. `fnl_output_name()` and
`fnl_gpu_name()` hand back per-thread snapshots taken under the lock, because
both buffers are rewritten by another thread and a `snprintf` over a longer name
leaves no NUL in the interval — an unterminated read, not a stale one. The
window-state bools read either a published copy under the lock or an atomic, and
so do `fnl_fps()`, `fnl_present_mode()` and `fnl_max_samples()`. A single bool
nothing else has to agree with looks like a safe exception and is not:
"harmless" describes the consequence, not the mechanism.

### The size field, and what it has to check

`FnlInitDesc` and `FnlCallbacks` each begin with their own `sizeof`, which is
the whole ABI story for the shared build: an older caller passes a smaller
struct, the library reads only the fields that existed then and defaults the
rest, and a size it does not recognise is refused rather than read off the end
of the caller's object.

Three things are checked, and the third is not obvious. Larger than this build
knows is `FNL_ERR_VERSION_MISMATCH`; smaller than the two fields every version
has had is `FNL_ERR_INVALID_ARGUMENT`; and **the size has to be a whole number
of fields**, which is tested as a multiple of the struct's alignment. Every
`sizeof` is one, so that rejects only values no build ever produced — and the
copy takes `size` bytes, so one landing inside a field leaves that field partly
written. The first field past the floor is a function pointer in both structs:
seventeen bytes gives the log callback one byte of an address and seven zeroes,
`fnl_set_log_fn()` installs it, and the first line through the sink jumps into
it. Worse than the read the other two checks exist to prevent, and from the same
caller-supplied number.

## The window

### Decorations

**This library draws the window's frame.** There is one kind of window and no
mode to choose between: nothing in `FnlInitDesc` selects a frame, and
`fnl_init()` opens the same window on every backend. The bar is a title, four
buttons (two where the window is the output) and a drag strip, laid out in
`src/core/decor.c` and drawn in `src/core/draw.c` out of the overlay's own text,
rectangles and strokes.

Each window server has to be told to stay out of the way, and each is told
differently:

| | how the manager's frame goes |
|---|---|
| Wayland | nothing to remove — xdg-shell's own default is client-side, and `zxdg_decoration_manager_v1` is asked for that mode where it exists |
| X11 | `_MOTIF_WM_HINTS` with its decorations field zeroed, which every manager implements and which keeps the taskbar button, alt-tab and snap that an override-redirect window would cost |
| Win32 | `WM_NCCALCSIZE` returns the proposed rectangle unchanged, so the window rectangle *is* the client rectangle; `WS_OVERLAPPEDWINDOW` stays, so the frame's behaviour outlives its drawing |
| Cocoa | a borderless `NSWindow` that keeps resizable and miniaturizable |

**One layout is the reason.** Six things are measured from a frame this library
draws: the bar itself, the grab ring, what `content_rect()` takes off the
window, the hit test, the size floor, and the strip `fnl_titlebar_reserve()`
hands back to a caller. A window wearing the manager's frame instead would need
a second set of all six kept in step by hand, and could only have one on some
backends — so the same program would look different depending on where
it ran. A window wearing no frame at all would need a third set, degenerate at
every one of those sites, for a case that in practice means fullscreen. One
window, one layout, every backend.

**Client-side is assumed, not negotiated**, and Wayland is where the assumption
can be refused. The backend asks `zxdg_decoration_manager_v1` for client-side
mode, but the protocol is explicit that the compositor's reply is what counts,
and there is no server-side layout here to switch to. A compositor that imposes
one gets a doubled titlebar; `decoration_configure` listens for exactly that and
says so through the log sink, which is the difference between a known limitation
and a mystery. Without the extension there is nothing to negotiate.

**Fullscreen is the one thing that takes the bar away**, and it is a state
rather than a mode — entered, left, and the bar is back on the way out. Every
site that needs to know asks `deco_client(fullscreen)`, and that single gate is
the point: a second reason to suppress the frame, should one ever arrive, has
one place to go instead of twenty. It takes the flag rather than reading
`App::is_fullscreen`, because the render thread asks about the state it latched
with the frame's size and not about whatever the input thread has since been
told.

### The titlebar controls

Beside the four window buttons the bar carries three checkboxes — `FSAA`,
`MLAA`, `Fs Upsample` (two buttons, and no `Fs Upsample`, where the window
is the output) — an output drop-down, a GPU drop-down and a size
drop-down. They are the library's, drawn in `src/core/controls.c`, and the
test for whether a control belongs there is simple: **it turns a knob the
library already owns.** All of them do — `fnl_set_antialias()`,
`fnl_set_scene_size_px()`, `fnl_request_size_pt()`, `fnl_request_output()`,
`fnl_request_gpu()` — so every application that wanted them would otherwise
write the same code against the same public calls.

They act through exactly those public calls, and that is worth stating because
it is what keeps the arrangement honest: a control here has no privileged route
into the library. What follows a click is the ordinary
`FNL_EVENT_ANTIALIAS`, `FNL_EVENT_SCENE_SIZE`, `FNL_EVENT_RESIZE`,
`FNL_EVENT_OUTPUT` or `FNL_EVENT_GPU`, produced by the frame's own
diff (see *The event queue*) rather than posted by the control. So a change made
by the bar and one made by the application arrive identically, and neither can
be missed by a control forgetting to announce itself. The other way round
holds too: the `FSAA` and `MLAA` boxes are drawn from the antialiasing mode the
frame latched, not from a tick of their own, so a mode the application set
with `fnl_set_antialias()` shows as ticked, and a click turns that mode off.
The scaling box keeps a tick of its own, since the size it holds is the
drop-down's pick, which nothing else records.

**The GPU box is always there.** Once the renderer has listed its GPUs a GPU
box stands before the size box, reading the GPU in use by its number (`GPU 2`)
and dropping a list of every GPU by number, name and kind; a pick is
`fnl_request_gpu()` (*Moving to another GPU*). With one GPU it is drawn dim and
does not open, as the size box with no size: the run, and the floor with it,
are then the same on every machine, and the bar says which GPU draws either
way. The number and not the name, because the box's width is the floor's: a box
wide enough for "GeForce GTX 1050 Ti" puts 181 points on it where "GPU 8" puts
69. The list is as wide as its widest row and hangs left from the box's right
edge.

**The output box is on every backend.** Before the GPU box stands an
output box naming the monitor the window is on, and its list is every
monitor: a pick is `fnl_request_output()`. On KMS that is the card's
outputs, the window taking the one picked at its preferred mode (*KMS*).
Under a window server the backend publishes its monitors and the one the
window is on whenever either changes (`app_publish_outputs()`) -- RandR's
monitors on X11, `wl_output`'s names on Wayland, `EnumDisplayMonitors` on
Win32, `NSScreen` on the Mac -- so the box follows a window dragged by its
bar, moved by the window server or by the API, with nothing of its own to
keep in step; a change is a frame owed, since two monitors alike in size
and scale change nothing else the frame compares. The move keeps the
window's size and state at the same offset into the new monitor's work
area, pulled inside it (`app_place_on_output()`, one rule for every
backend). X11 asks with `_NET_MOVERESIZE_WINDOW` as a pager, at
`StaticGravity`, where a manager may place an application's own
`ConfigureRequest` itself; a maximized or fullscreen window leaves the
state, moves and takes it again. Win32 moves a maximized window's
restored rectangle through its placement -- which is where a restore
lands -- and Windows does not move the window itself with it: measured on
2026-11-05 (Windows 11 26H2), a pick put the restored rectangle on the
other monitor, left the window maximized on the one it was on, and a
restore brought it to the picked one. So the state is left and taken
again there too: the restore follows the placement, which lands it on the
new monitor, and the maximize follows the restore. Wayland's clients place
nothing: the one output a client names is a fullscreen window's
(`xdg_toplevel.set_fullscreen`), and the bar is not shown in fullscreen, so
there the box only names and follows, drawn dim
(`outputs_fullscreen_only`), and `fnl_request_output()` moves a fullscreen
window alone. A fullscreen window on the Mac is a Space of its own, entered
and left through AppKit's animation, and is not moved.

Under a window server the box is **the one control that gives way.** Its
109 points on the run would put the floor at 734, past `CONTENT_MIN_W`,
and empty the size list of 640x480 (*The size floor*); so it joins the run
where the window is 734 wide or more (`ctl_output_in()`), and the floor is
the run without it. On KMS it is always there, and in the floor.
Like every rectangle here it is a function of the window width alone.

Measured on 2026-11-05 on X11, captured at three widths: the box is drawn
between the checkboxes and `GPU 1` at 740 points, is still drawn at 734,
and is gone at 732, where the GPU box has taken its place.

**The size list is derived, not shipped.** A hand-written list of resolutions
would be a product decision, and the library has no business having one. What
`controls.c` holds is a table of *standard* sizes filtered against two figures
the library already publishes — its own size floor and the room the output
actually has — so the list a user sees is a fact about their monitor. On a small
display it is short; where every entry is filtered away the box is drawn dim and
is inert rather than merely ineffective, which is the same rule the `FSAA` box
follows on a device offering no sample count worth having.

**The open list is modal, and that is the one thing owning it bought.** Every
drop-down convention consumes the click that dismisses it. A caller could not:
events are delivered alongside the library's own hit test rather than instead of
it, so a dismissing press in the presentable area *also* captured the pointer —
the cursor vanished and a reticle appeared, from a click that meant "never
mind". While the list is open, `ctl_modal()` routes every press to the controls
wherever it lands, and the dismissing one stops there.

The drop-down reaches below the bar, so it is hit-tested before the resize edges
and the presentable area — **drawn last, hit first** — and both halves of that
have to be arranged rather than assumed.

Hit first is the order `deco_hit()` tests in, which is every backend's hit
test: `ctl_modal()` ahead of the resize edges, the edges ahead of the bar. So a
press on the resize margin under an open list is the list's on every backend.
Drawn last is the interesting one, because "over" is an *append order* here:
the three streams are issued once each, by kind (see *Recording*), and whatever
was appended last is on top.
The bar is painted before `on_overlay` runs — so a list drawn with it is under
everything the application then draws, and the flush that stands at the top of
the list would only ever separate it from the bar, because at that point in the
frame the application has drawn nothing. That is not cosmetic while the list is
modal: a caller's panel over an invisible list swallows the click that would
have put it away.

So the list is a second entry point, `ctl_draw_popup()`, called from the end of
`draw_overlay()` after the callback — with the metrics and calibration panels,
what this library draws over a caller's drawing rather than under it. The flush
inside it is what puts the application's primitives underneath rather than
beside, and it flushes the glyphs too. Being the last appended into the frame's
text is not enough: the list's background is a rectangle, issued before that one
text draw, so the caller's text came out over it -- `fenestal-minimal`'s readout
read through the open GPU list. `text_end()` issues what was appended since it
last did, as the rectangles' flush does (`firstInstance`, or the buffer offset
on Metal), so the list costs the frame one text draw more while it is open.

A caller that wants the bar to itself replaces it outright, and then the library
draws no controls either; that is the next section.

### Replacing the titlebar

The bar above is the library's, and a caller can take it.
`fnl_titlebar_custom()` states a height and a width floor; from then on the
library draws no bar, the
caller draws whatever it likes across the top of the window, and the geometry
follows — `content_rect()` starts below the caller's bar, the window is resized
so the presentable area asked for is the one still there, and
`fnl_content_rect_pt()` reports it.

**What the library keeps is the behaviour, and that is the whole design.** A
press anywhere in the band still drags the window through the window server's
own interactive move; the resize edges still work; a double click still
maximizes; the drag still waits out `DECO_DRAG_SLOP` so a plain click does not
nudge the window. None of that is reachable from outside — `HTCAPTION`,
`_NET_WM_MOVERESIZE` and `xdg_toplevel.move` are answers a client gives its
window server, and a caller reimplementing the reachable half would get a bar
that behaved almost but not quite like every other window on the desktop.

So the seam is declarative. `fnl_titlebar_claim()` names a rectangle as the
caller's, and everything unclaimed in the band stays drag strip:

| a press lands on | zone | what happens |
|---|---|---|
| a claimed rectangle | `ZONE_CLIENT_ITEM` | the caller's event, and nothing else |
| anything else in the band | `ZONE_TITLEBAR` | drag, or double-click to maximize |
| the grab ring or margin | `ZONE_RESIZE` | the window server's resize |
| a claim below the bar (`fnl_claim()`) | `ZONE_CLIENT_ITEM` | the caller's event, and no capture |
| anything else below the bar | `ZONE_CONTENT` | the capture (*Pointer capture*) |

**`fnl_claim()` is the same seam over the whole window**, for a panel drawn
over the scene -- the testbed's benchmark mode is one (`docs/benchmark-mode.md`)
-- that wants the mouse without the capture a click there would start. It
shares the list, and in a bar of the caller's it is `fnl_titlebar_claim()`.
Over the library's own bar it is inert: `deco_zone_at()` answers for the band
without reading claims, so the buttons, the controls and the drag strip stay
the library's whatever a caller claims. The rejected alternatives were the
library's controls made public, which are the bar's layout and act through
the library's own knobs (*The titlebar controls*), and a switch that turns
the capture off, which takes it from the whole window when the benchmark wants
a panel and a captured run in the same one. The list is published every
frame, whoever owns the bar, and the scan costs a motion in the presentable
area a lock and at most 64 comparisons.

**Claims are rebuilt every frame**, from inside `on_overlay`, beside the drawing
they describe. That is what keeps the hit test honest: the rectangle handed over
is computed by the same code that drew the control, so there is no frame in
which something is clickable where it is not shown, and a control that stops
being drawn stops being claimed in the same frame. Registering rectangles once
at startup is the bug this shape avoids — the window resizes, the controls move,
and the claims stay where they were. The list is swapped into `App::pub_claims`
under the lock the published figures already take, whole rather than
incrementally, so a hit test cannot catch it half-rebuilt. 64 per frame,
because this runs in the frame path where an allocation is a stall; past that
the extra is dropped with one line through the log sink.

`fnl_begin_move()` and `fnl_begin_resize()` are the imperative half, for a
caller that wants to decide for itself — a bar with its own gesture rules, or a
resize grip drawn down in the scene. They hand the pointer to the window server
from the press currently held, which is why they must be called from a press.

**A replacement bar owns the buttons too.** There is no minimize, maximize,
fullscreen or close in a bar the library did not draw, so `fnl_minimize()` and
`fnl_set_maximized()` join `fnl_set_fullscreen()` and `fnl_quit()` as public
calls. `fnl_titlebar_reserve()` refuses once the bar is the caller's: it lends a
run of the library's bar, and there is no longer one to lend.

**The floor is the one number the library cannot work out.** Its own
`DECO_MIN_W` comes off the button strip it drew; it cannot measure a bar it does
not draw, so a custom bar states its own width floor and gets no other
protection. The height floor is always the bar's own height, for the reason it
always was: a window shorter than its titlebar has no titlebar.

The window outline stays the library's either way. It is the window's edge and
not the bar's, and a black presentable area needs one to be seen against
whatever is behind it.

`testbed/titlebar_custom.c` is a worked replacement, run with
`--custom-titlebar`. It is written against `fenestal.h` and nothing else, which
is what makes it a check on the seam rather than a demonstration of it.

**A renderer that cannot draw the bar is refused, not degraded.** The titlebar
is built from the overlay's primitives, and `FnlRendererVtbl::draws_overlay`
says whether a renderer has them; a `false` there is `FNL_ERR_UNSUPPORTED` out
of `fnl_init()` with no window opened at all. It has to be refused there,
because the backend's caption is taken away at creation to make room for a bar
that would then never be drawn: a window with no caption from the window server
*and* none from here, and nothing to close it with but the keyboard.

### The presentable area and the grab ring

The window opens at whatever size gives a **1280x720 presentable area** at the
desktop's scale; the window comes out taller by the titlebar. Or at the room
there is, where that is less: on Wayland and X11 a window manager answers a
size it has no room for with another, and on Win32 nothing does, so there the
library fits the opening window to its monitor's work area itself
(`win32_open()`). A panel of 1920x1080 at 150% is 1280x720 points before the
taskbar, and the default opened 1920x1131 pixels on it, the bottom of the
window and its right-hand border where no pointer reaches. It opens 1920x1008
there now, the work area, with 1280x638 presentable. X11 with no manager is
Win32's case and is fitted the same way, before the map (`fit_opening()`):
the Raspberry Pi's forced 1024x768 output opened the default 1280 wide, the
maximize button and the right-hand border past the screen's edge. Only the
opening size: one asked for afterwards is the caller's to have filtered
against `fnl_output_size_pt()`.

The titlebar is the only thing the presentable area gives up: on the other three
sides it runs to the window's edges, exactly as it does in fullscreen. There is
deliberately no border. A border painted in the same black as the area itself
cannot be seen, so every point of it reads as an error instead — the crosshair
of a captured pointer stopping short of the window edge in windowed mode while
reaching it in fullscreen.

On Wayland the window is resized by a transparent ring **outside** it: the
surface is `DECO_GRAB` points bigger on every side, and
`xdg_surface.set_window_geometry` tells the compositor which rectangle is the
window, so it still positions, snaps and tiles by that. The ring is cleared to
zero and the swapchain composites pre-multiplied, so it is invisible; the opaque
region covers the window only. Fullscreen has no ring.

Putting the grab outside is what makes all four edges behave alike — there is
nothing to grab *inside* a titlebar — and it stops the resize strip from eating
into what the window presents.

X11, Win32 and Cocoa have no such separation: the window rectangle *is* the
window, and X11's and Win32's window managers own a resize border already. There
the edges are an **inner** `DECO_GRAB` margin, hit-tested by the same
`deco_hit()` and drawn over by the scene — the same target in the same place on
screen, on the other side of the line. `FnlBackendVtbl::grab_ring` is what says
which arrangement a backend wants; adding the ring unconditionally makes the
buffer twelve points larger than the window and offsets everything drawn in
window coordinates.

A titlebar press only becomes a window move once the pointer has travelled
`DECO_DRAG_SLOP`. Handing it to the window server on the press itself gives the
pointer away on every plain click, and leaves the press that started a drag
counting as the first half of a double click. The arming, the slop and the
double click are `deco_press()`, `deco_drag()` and `deco_release()` in
`src/core/decor.c`, once for every backend; what a backend adds is the request
its window server takes.

On Wayland that request wants the serial of the press that began it, and the
backend keeps the left button's apart from the last press of any button
(`left_serial` while `left_held`, read through `grab_serial()`). One serial for
every button is the simpler arrangement and is wrong during exactly the slop: a
second button pressed before the pointer has travelled far enough replaces the
serial, and the compositor drops a move that names a press which did not begin
the grab.

### The size floor: two figures, one enforced

**Advisory.** `CONTENT_MIN_W` x `CONTENT_MIN_H` is 640x480 logical points of
presentable area: the smallest size worth *offering*. It is what
`fnl_min_content_size_pt()` reports and what a caller filtering a list of sizes
should filter against. It is not a size the window refuses to go below.

**Enforced.** `deco_min_window()` is **556x34** points of window geometry (583
wide where the window is the output, whose run has an output box and a wider
mode box; each with the GPU box's 69 in it once a renderer has listed its GPUs:
625 and 652; under a window server the output box is not in it, and gives way
below 734 instead -- *The titlebar controls*), and both figures come straight
off the decorations they protect:
the width is one point above where `ctl_run_x()` drops the controls' run — the
button strip (`DECO_MIN_W`, 157), the group gap and the run — and the height is
the titlebar itself. Below either, the window loses a control, a close button
or the bar to grab. The title is not in it: it is the caller's string, and one
naming the platform and the device (`fnl_gpu_short_name()`) is half a monitor
wide, so a floor that held it would move with every `fnl_set_title()` and
refuse sizes worth having. The presentable area is allowed to be nothing at all
— `content_rect()` clamps it to a point — and that is the trade: a window
dragged to the floor is all chrome and no content, which is recoverable.

Static asserts pin `DECO_MIN_*` to the constants they are derived from and
`CONTENT_MIN_*` above them; the run's width is the baked font's advance and
no constant, so `deco_min_window()` asserts at run time, in a Debug tree,
that the floor stays under `CONTENT_MIN_W` — above it the size list, which
is filtered against the floor, would offer nothing. Not where the window is
the output: its list is the output's modes, which the floor does not
filter, and its 652 is past 640.

The floor grows once after `open()`: the GPU box joins the run when the
renderer's first pick publishes its list, and the window server, told the
floor at open, is told it again then (`apply_size_bounds()` from
`fnl_init()`); missed, X11's manager held the window to 556. Measured by the
round (`stress.py`, the corner carried to the window's own left edge): 625
on X11 for both renderers, on Win32 on both laptops and under wine's X11
driver. Wine's Wayland driver lets the compositor take the window to 119,
below what the Win32 backend arms after a resize, and is declared
(`FLOOR_UNHELD`). With no window manager (bare X11) the library resizes
its own window in steps of 16 pixels and keeps the floor to the step,
640 (`FLOOR_STEP`). On the Mac the borderless window is resizable, which
fullscreen needs, and AppKit's own edge resize took it to 43 points: it
holds the window's `minSize` and nothing else, which `hold_floor()` now
sets at open, with the scale and whenever the floor moves; 625 since.

The floor applies only where this library draws the titlebar, which is every
windowed frame. In fullscreen there is nothing to keep grabbable, the size is
the monitor's, and no floor is asked for.

*When* it applies differs by backend, and the difference is deliberate:

| | mechanism | behaviour |
|---|---|---|
| Wayland | `xdg_toplevel.set_min_size`, enforced again on the configure | the drag stops |
| X11 | `WM_NORMAL_HINTS`, honoured by the manager during the drag | the drag stops at exactly the floor (625x34 once the GPUs are listed) |
| Win32 | a timer grows the window back after the drag, armed from `WM_SIZE` and again from `apply_size_bounds` when the floor itself moves | the drag goes below and springs back |

`ptMinTrackSize` is deliberately **not** set on Win32. Bounding the resize means
the client's idea of its size and the window's can disagree while the size is
changing, which under wine painted a 34-point titlebar into a one-pixel window
thirty times a second. Nothing bounds the drag, so the two cannot diverge; the
timer asks `GetAsyncKeyState` for the mouse button to tell a finished drag from
a paused one. `WM_EXITSIZEMOVE` is not usable for this — `WM_ENTERSIZEMOVE`
arrives reliably and its partner does not, and a bracket whose closing half can
go missing would leave the floor switched off for the window's life. One refused
grow-back is taken as the answer until the window is next seen at or above the
floor.

### Give-way rules

With the floor this low, every layout threshold above it is reachable, so each
one has to be a *give-way* rather than an assumption. The rule, throughout:

**buttons and the library's controls never yield (but for the output box under
a window server, below 734); the title yields to those; a
caller's run yields to all three; a panel you can turn off yields to one you
cannot; everything yields to the titlebar.**

So a narrowing window loses the caller's controls first, then the title, and
never the library's controls or the close button, which the floor holds. The
title goes before the controls because it is a label, already on the window
server's own lists, where a checkbox gone is a knob the user cannot reach.
The first three are the library's and are contract:
`fnl_titlebar_reserve()` answers false rather than handing back a rectangle that
would overlap what is already there, which is how a caller reads the rule
instead of inferring it. Calling it every frame from `on_overlay` is the
natural way to use it, so it stores the width only when it moved: the pump's
hit test reads the same word. Measured on 2026-11-05 through the testbed's
`--reserve-log`: with its title, a 120-point caller's run is invisible until
the window is about 1300 points wide -- the run yields to the title long
before the output box gives way at 734 -- and where it is visible its left
edge holds 855 points in from the window's right edge as the window widens.
The last two are a caller's own, and `testbed/overlay.c` is one worked answer.

Every rectangle in the bar is a pure function of the window width, which is what
lets both threads compute it — the render thread to draw and the input thread to
hit-test — with nothing shared between them, so a control can never be drawn
where it cannot be clicked.

A layout figure derived from a measurement wants **re-measuring whenever
anything joins the row it is measured against**. Adding one control to the
titlebar group moves its give-way threshold by the control's full width, and
nothing looks wrong until the margin has been spent: the failure is silent right
up to the point where it is a collision.

Two thresholds are worth naming because their cost is not cosmetic, and both
come from the overlay's ordering invariant (see *Recording*): a rectangle meant
to sit *over* something has to flush first or it lands underneath. An open
drop-down reaching back over the help panel is one; the help panel colliding
with the status readout is the other, and there the help panel gives way — it is
the half `F1` turns off, and a reminder that covers the thing it is reminding
you about is worth less than nothing.

### Scale

The scale is the desktop's: `wp_fractional_scale` on Wayland where available and
the output's integer scale otherwise, `GetDpiForWindow` on Win32, the `Xft.dpi`
resource on X11. It is carried as **120ths** — 120 is 100%, 180 is 150% —
because that is exact where a float is not, and every layout figure is derived
from it by integer arithmetic.

It is a **render scale**, not a DPI setting, and it holds in fullscreen as well
as windowed: the surface keeps whatever logical size it was given and only the
buffer behind it changes resolution.

`--scale=<n>` overrides it, from 1 to 8. The **window keeps the size it would
have had** and only the buffer grows: `--scale=8` is a 1280x754-point window
over a 10336x6128 buffer, not a 160x124-point one. Sizing the window from the
override instead made a high scale open a window a fifth of the width asked for.
Below 1.0 is refused: the buffer would be smaller than the window stretched over
it, and everything the library draws is laid out in points and rounded to whole
pixels, so the decorations end up with fewer pixels than they have features.

On X11 the scale is read from `Xft.dpi` on the root's `RESOURCE_MANAGER`, where
96 is 100%, and re-read on the `PropertyNotify` that says it changed.
Deliberately **not** derived from the monitor's physical size, which RandR does
report: a DPI computed from millimetres is right on a laptop panel and absurd on
a television that reports 160x90 mm.

Every scale that arrives from outside — `FnlInitDesc::scale_override120`,
`Xft.dpi` out of the root's resource database, `wp_fractional_scale`'s
preference and `wl_output.scale` — is first held to **`SCALE120_MIN` to
`SCALE120_MAX`** in `app.h`, which is 0.5 to 8.0. All four are somebody else's
number and every layout figure is multiplied by whichever comes out, so an
absurd one is a signed overflow in `create_swapchain()` rather than a window at
an odd density — and at startup there is no device yet whose ceiling the clamp
below could cut it against. The floor is 0.5 rather than the 1.0 this section
refuses `--scale` below, because 0.5 is a figure the library itself may arrive
at: the device clamp stops there. A bound that refused what the library produces
would be a bound in two minds. The descriptor and the compositor are clamped;
X11's resource is refused outright, since a desktop that publishes nothing
already has 100% to fall back on.

A scale whose buffer is past the device's `maxImageExtent` is **cut back to one
that fits**, in `clamp_device_scale120()`, before the size it belongs to is
published. Cutting the *scale* rather than the *extent* is the point: the
overlay converts the allocated extent back to points, so clamping the extent
alone leaves the layout drawing for a window the buffer no longer matches —
every decoration too large, and a reticle clamped against the scale but drawn
against the extent running off the end of the buffer. The extent clamp remains
as the backstop for a ceiling that moves between the resolve and the build, and
it brings both axes down **proportionally**, one factor, because clamping them
independently distorts rather than losing detail evenly.

Only Wayland has a ceiling to bind. There the window is negotiated in points and
the scale multiplies the buffer behind it. On X11 and Win32 the window is
physical: its pixels are the server's, the buffer is exactly as many of them
whatever the scale, and the scale decides how many *points* that is.

Layout runs in logical points throughout, so the overlay converts back from the
extent that was actually allocated — **rounding to nearest**, since `scale_pt()`
rounds on the way out and truncating on the way back loses a point whenever that
rounding went up.

The scale is resolved and published in the same locked update as the size, and
after that size is settled, since the device ceiling bounds the buffer.
Published a beat apart, the frame caught between them draws the new size at the
old scale.

### Outputs

`wl_surface.enter` / `leave` are tracked as a **set**, not as one current
output: a window straddles two monitors for as long as a drag between them
takes. The app follows whichever has the highest scale. A surface on no output
at all keeps its last choice rather than falling back to 1.0, so coming back
costs no rebuild.

That choice decides the scale, the size bounds and the output the window
reports, and nothing else. `F11` names
**no** output: `xdg_toplevel.set_fullscreen(NULL)` leaves the screen to the
compositor, whose answer for a straddling window is the one it is mostly on —
which is also the one it will restore to. Naming the followed output there is a
category error: two monitors of equal scale are always a tie broken by registry
order, so a window almost entirely on one screen fullscreens onto the other, and
leaving fullscreen restores it there.

The room a window has to fit inside comes from `xdg_toplevel.configure_bounds`
(xdg_wm_base v4 and up, with the output's mode standing in when the compositor
gives none), `_NET_WORKAREA` intersected with the monitor the window is on for
X11, and `MONITORINFO::rcWork` on Win32.

X11 monitor names and rectangles come from RandR 1.5 `RRGetMonitors` — the
connector, `HDMI-1`, the same string `wl_output` reports. A server without it
falls back to the screen, which on a multi-monitor display is the *bounding box*
of every monitor and overstates the room by whatever lies between them.

The X11 window's position is tracked rather than asked for. A reparenting
manager puts the client inside a frame, so a non-synthetic `ConfigureNotify`
carries coordinates relative to *that*, and `TranslateCoordinates` would be one
round trip per step of a drag. The frame is watched instead — its own
`ConfigureNotify` carries the root-relative origin — and the two are added.

**A change of output is an event**, `FNL_EVENT_OUTPUT` (1.1): diffed once a
frame in `events_post_state()` like the other notifications, by the name
and by the size, since two monitors alike in size are still two. The event
carries the size and not the name, which has no upper bound an event could
hold; `fnl_output_name()` is how it is read, as the clipboard's text is.
The round checks it beside the move between outputs. Wine on Wayland never
says the window moved, so it posts none there either.

### Pointer capture

Five backends, five mechanisms, one model: the application owns where its
reticle is, and the pointer stops reporting a position and starts reporting
movement.

| | how |
|---|---|
| Wayland | locks the pointer, which then does not move at all, and `zwp_relative_pointer_v1` delivers relative motion, its unaccelerated delta taken |
| Win32 | clips the cursor to the window and takes deltas from raw input, because a clipped cursor stops moving at the edge |
| X11 | grabs the pointer confined to the window and takes each delta from the previous motion event's landing, warping the pointer back to the middle only once it has left the middle half of the window |
| Cocoa | detaches the cursor from the mouse (`CGAssociateMouseAndMouseCursorPosition`) and hides it, and the events' deltas are the movement (*macOS*) |
| KMS | hides the cursor it draws itself; evdev's relative motion is the movement, as it is when not captured |

**The movement is the hand's where the system has it.** Wayland's
unaccelerated delta, Win32's raw input and KMS's evdev are the device's
counts before any pointer curve, which is what a camera turned by the mouse
wants: the same flick turns it the same angle however fast it was made.
X11 and Cocoa report the desktop pointer's accelerated movement and nothing
under it -- core X has positions alone, its raw motion being XInput 2's,
which the X11 backend does not encode, and AppKit's deltas are the cursor's
-- so there a caller's camera turns on the desktop's curve. A Wayland
compositor with no unaccelerated figure for a device sends zeros beside a
movement; the accelerated delta stands in, and the log says so once.

**The confinement goes with the focus; the capture does not.** Win32 lifts
its clip and shows the cursor, Cocoa associates the mouse again, X11
ungrabs and gives its window an arrow, and Wayland's compositor suspends a
persistent lock by itself; each takes it back with the focus, and the
reticle waits where it was. A confinement outlasting the focus holds the
pointer over a window the user switched away from: `ClipCursor` is global,
and an X grab is the server's. Ending the capture instead would make every
alt-tab a release. X11's grab can be refused at the focus-in while a
manager's switcher still holds the pointer; it is tried again at that
manager's keyboard ungrab and at the next motion over the window, and the
deltas start from where the grab found the pointer, not from where it was
left. A release while suspended puts nothing back: the pointer is the
user's, elsewhere.

There is deliberately no `fnl_warp_pointer()`: Wayland forbids one outright, and
releasing a capture there can only *hint* where the desktop cursor should
reappear. X11 warps exactly, so the cursor comes back precisely where the
reticle was, and so do Win32 (`SetCursorPos`) and Cocoa. On Win32 that is
not cosmetic: the hidden cursor moves by Windows' acceleration and stops
where the clip stops it, so left where it was it can stand on the bar or
an edge, and the next press there -- in a VirtualBox guest, one the
emulated mouse delivered late -- enters Windows' sizing loop. Measured
in the win10 guest on Direct3D 12, where the round pushes the hidden cursor
to the bar's top-left before F9: left there, two rounds of four lost the
window to that loop on the top edge, its first height the old one less the
y of the round's next report; put back at the reticle, the cursor came
back at (534, 241), in the scene, and eight rounds of eight passed, with
win11 and Vulkan beside them (2026-10-29). The X11
warp generates a motion event of its own, recognised by landing exactly on
the middle; that drops one genuine motion that happens to end there, which
is the standard trade.

**What the warp is not for.** The obvious arrangement — warp after every
motion, take the delta from the middle — is wrong, and wrong in a way a driven
harness cannot see. A warp is a *request*; the server runs it when it reaches
it, and every motion event already in flight when it was sent carries a
position from before that happened. Measured from the middle, each of those
re-counts the whole distance the ones before it already reported: a pump turn
that collected `n` motion events moves the reticle `(n+1)/2` times too far, and
`n` is however many that turn happened to collect. At one event per turn — what
`stress.py` produces, because the harness drives one step and reads back — the
factor is exactly 1 and nothing looks wrong. At a 1000 Hz mouse and a 60 Hz
pump it is around 8, varying turn to turn: mouse-look far too fast, and
differently so every frame.

So the delta comes from the previous event's landing, where the positions
telescope and the arithmetic does not care when the warp lands. The warp is
then only what it was ever needed for — keeping a confined pointer away from
the window edge it would otherwise stop at. It goes out when the pointer leaves
the **middle half** of the window, and is flushed on the spot rather than at
the end of the pump: that leaves a quarter of the window in the direction of
travel as room for the events still in flight, and a warp sitting in the output
buffer behind a burst of motion is a warp the pointer spends that whole burst
travelling without. A confined pointer that reaches the edge reports nothing at
all, which is a reticle that stops dead mid-flick.

There is one way in, and no way out by the pointer. Clicking into the
presentable area captures and starts the reticle where the click was, unless the
caller claimed that point for a control of its own (`fnl_claim()`); clicking the
titlebar does not. `F9` releases, and so do `fnl_release_pointer()` and exiting.
Splitting them that way means nothing the pointer does can end a capture by
accident, and nothing the keyboard does can start one over a window the user was
only pointing at. There is deliberately no call that *takes* the pointer: a
library that could grab it unprompted is one that will.

The Wayland lock is created with **no** region. A lock's region says where the
pointer must be for the lock to *engage*, not where it may then go — a locked
pointer does not move at all — so it would buy nothing. Bounding is the
reticle's business.

**The reticle is the core's, once for every backend**
(`app_reticle_start()`, `app_reticle_move()`, `app_reticle_clamp()` in
`src/core/app.c`); a backend supplies deltas in pixels and nothing else. It is
kept in fractional pixels, bounded to the area's last pixel on every side, and
published **rounded to the nearest pixel and only when that pixel changes** —
so a captured motion takes `App::lock` when the reticle moves on screen rather
than once per event, and a 1000 Hz mouse does not contend with the frame latch
a thousand times a second.

**Two units, and they are not interchangeable.** The reticle the library tracks
(`App::cur_px`) is in **pixels of the presentable area**, because that is the
space it is clamped in and the only one where "every pixel and no more" means
anything. `FNL_EVENT_RELATIVE` is in **logical points**, because every
*position* a caller is given is in points and a caller integrating deltas to
move its own reticle needs the same space. Wayland delivers points; X11 and
Win32 deliver pixels and convert for the event. They are the same number at 100%
and nowhere else, which is what makes the mistake invisible until it is not.

The size the reticle is clamped to has to be the one the render thread
actually draws, which is what `app_content_px_rect()` answers: both edges are
rounded after the titlebar and the grab ring have shifted them, so at 1.25 and
1.75 the span differs by a pixel from the same two edges without them.

The reticle is re-clamped on **every** configure, not only on a fullscreen
toggle: a drag-resize shrinks the area under it continuously, and a size picked
from a caller's own list does it in one step. Clamped and deliberately never
re-centred — what `App::cur_px` decides is where the *desktop* cursor is put
back when the capture ends, and the reticle a user is looking at is the
caller's, moved by the same deltas from its own copy. Moving one and not the
other makes them disagree: the caller's reticle stays where it was and the mouse
pointer reappears in the middle of the window. Only while captured; with the
pointer free the desktop owns the cursor.

A caller keeping a reticle of its own has the matching problem one level up,
and it is told in time: `FNL_EVENT_RESIZE` is posted ahead of the callbacks of
the first frame built at the new size, so a drain at the top of `on_overlay`
sees it before anything is drawn there. A position kept in pixels therefore
*can* be right — by re-clamping on every one of those events, and a caller that
misses one draws its crosshair outside the area, where it is clipped away:
a captured pointer with no cursor and no crosshair anywhere on screen.
`testbed/overlay.c` stores its crosshair as a *fraction* of the
presentable area instead, which needs no handling at all: a fraction means the
same thing at any size, so a fullscreen toggle, a drag-resize, a scale change
or a pick from the size list all leave the crosshair where it was relative to
the picture.

Capturing needs both Wayland protocols; without either the click is an ordinary
click. The lock alone is not worth taking: it freezes the pointer and the
reticle is moved by nothing but the relative reports, so locking without them
hides the cursor and draws a reticle that cannot move.

### The event queue

Input is a queue an application drains, not a callback it is called through:

```c
FnlEvent ev;
while (fnl_poll_event(app, &ev)) { ... }
```

`on_overlay` is where to drain it. That is the turn in which an application
lays out and draws what the input changed, so draining there answers a click
with the *same* frame rather than the one after it.

**A queue, because a callback costs every caller a lock.** A callback runs on
the pump's thread, and the state a click changes is the state the next frame
draws from — two threads with nothing between them. The library cannot lend its
own lock, since a caller's hit test would then contend with the frame latch, so
every caller would need one of its own. Draining in `on_overlay` collapses that
to one thread: input and drawing are the same turn, so there is nothing to
share. `testbed/overlay.c` is the worked case — its state is plain
fields, with no lock and no atomics.

**One stream carries both halves.** Beyond input, the queue carries what the
*library* did — `FNL_EVENT_RESIZE`, `FNL_EVENT_SCALE`, `FNL_EVENT_WINDOW`,
`FNL_EVENT_ANTIALIAS`, `FNL_EVENT_SCENE_SIZE`, `FNL_EVENT_CAPTURE`,
`FNL_EVENT_OUTPUT`, `FNL_EVENT_GPU`. A click on a
titlebar control and the antialiasing change it caused are one story, and
splitting them across two mechanisms would leave an application correlating them
by hand.

Those notifications are **diffed once a frame** in `events_post_state()` rather
than posted at each site that could cause one. The sites are spread across the
backends, the frame loop and the public setters, and a notification added at
each is one missed at the next that somebody adds — but every one of them shows
up in the figures the render thread publishes, so the diff runs there and cannot
be bypassed. The diff is taken as each frame's figures are published, ahead of
that frame's `on_record` and `on_overlay`: a change the window server made
arrives with the first frame built from it, and a change a caller makes from
inside a callback is reported on the frame after, which `fnl_poll_event()`
states rather than hides.

**Where to drain depends on what the input moves.** `on_overlay` is right for
anything the overlay draws, and a frame late for anything the scene draws,
because `on_record` has already run. A program whose input moves its scene
drains at the top of `on_record` and again in `on_overlay`; they are the same
thread. A waiting event brings a frame if either callback is registered.

**Motion coalesces; nothing else does.** Consecutive `FNL_EVENT_MOTION` collapse
to the newest position and consecutive `FNL_EVENT_RELATIVE` sum their deltas, so
a 1000 Hz mouse against a 60 Hz drain cannot flood the ring — and neither loses
anything, because the newest position and the total movement are what those two
events *mean*. Merging a button press with anything would lose an edge, so
nothing else is merged.

**Overflow drops the newest and says so.** Dropping the oldest loses the start
of a gesture and keeps its end, which is how a release arrives for a press
nobody saw. The count comes back as one `FNL_EVENT_LOST` in the stream, so a gap
is reported rather than inferred. The ring holds 255 (256 slots, one kept
free), which is many frames of
everything that does not coalesce.

The queue has a mutex of its own rather than sharing `App::lock`. That one is
held across the frame's whole latch, and an input burst waiting on it would put
pointer latency behind the render thread's turn. It also starts a cache line
of its own in `App`, with the ring's indices: the pump takes it per event, and
sharing a line with atomics the render thread stores would have each thread
taking the line from the other.

**The input enums are pinned by value.** `FnlKey`, `FnlButton`, `FnlEventType`,
`FnlAntialias` and `FnlCursor` all write their numbers out. A caller stores
these in its own config and links against a shared object it did not compile,
so a value that moved because a member was inserted in the middle would
silently remap somebody's keybindings. Append, never insert, never renumber; a
type an application does not recognise is one from a newer library than its
header, and is ignored. `FnlButton` names left, middle and right; any other
button posts no event on any backend.

### Input latency

**From the hand to the drain.** `fnl_input_latency_ms()` is how long an
input takes to reach the caller: from when the key went down, the button,
the wheel or the pointer moved, to when `fnl_poll_event()` hands it over,
which is the input's arrival in the scene. It covers the window server's
delivery, the pump's wake and dispatch, and the wait in the queue for the
next drain, which on a window drawn on vsync is most of it. What comes
after -- the recording, the GPU, the present and the scan-out -- is the
frame's, and the two cost figures beside it say what that costs. Measured
to the photon instead it would want a signal of the present that only
Wayland's presentation-time gives, and what an input changes in the
picture is the caller's scene, not the library's.

**The start is the input's own stamp, where the library shares its
clock.** Each backend turns the stamp into an age at the moment it reads
the input (`app_input_age()`), from a clock of its own system and no
other, and core takes the age from now:

- Wayland's `wl_keyboard` and `wl_pointer` times, and the X server's, are
  milliseconds of `CLOCK_MONOTONIC`, wrapping in 32 bits; the age is the
  difference in 32 bits, from the middle of the stamp's millisecond
  (`loop_input_ms()`). The relative pointer's are microseconds.
- evdev's, under KMS, are `CLOCK_MONOTONIC` too, which `EVIOCSCLOCKID`
  already asks for the double-click (`loop_input_us()`).
- Win32's `GetMessageTime()` is on `GetTickCount()`'s clock, in its steps
  of about 16 ms whatever `timeBeginPeriod()` asks: one input reads as a
  whole number of steps, and a mean over many reads through, since both
  readings fall anywhere in theirs. So on Windows itself the time comes
  from raw input instead (below), and the tick is what is left where that
  will not run. Under wine `GetTickCount()` counts milliseconds, and the
  tick is the time.
- Cocoa's `NSEvent.timestamp` is the system's uptime, as
  `NSProcessInfo.systemUptime` is, stamped as the pump dispatches it --
  a key, modifier, mouse or wheel event only, as Win32 stamps its key and
  mouse messages only (`is_input_event()`): an AppKit- or system-defined
  event's age is not an input's.

Wayland does not say which clock its times are on. An age past a second
(`INPUT_AGE_MAX`), or below zero by more than the stamp's rounding, is
taken for a clock the library does not share, and that input counts from
when the pump read it, which leaves out only the server's delivery; in 2^32
milliseconds a foreign clock lands within the second by chance once in four
million. A stamp is the next input's that the backend hands in within 2 ms
(`INPUT_STAMP_FRESH`), one handler's work, so that an input with no stamp
of its own -- the motion a Wayland enter posts -- never takes another's.

**What counts, and once.** A key's press and release, a button, a wheel
step, a motion: what a hand did. Not a repeat, which the library or the
server times; not text or a composition, which the key that made it has
counted; and no notification. Coalesced motion counts each movement it
carries (`App::ev_born`): the newest alone would read short by however
long the queue held the rest.

**Published as the costs are.** The mean over the inputs handed over since
the last figure, published with the cost figures' window -- once a
`COST_WINDOW` while frames are drawn, at once where there was none, and
at rest after one (`events_latency_publish()`) -- into the draw state, so a
change re-records the frame that shows it; kept while no input arrives. A
re-record for it alone is a figures frame, as theirs is, and owes no
other. `--frame-times` logs each figure, `input 8.58 ms over 13 inputs, 13
stamped, longest 15.84`: how many were stamped says whether the server's
clock was the library's.

Measured on schenker under weston, the testbed on Vulkan on the GTX 1050
Ti, 60 Hz, by uinput: every input stamped; a key or a button 0.3 to 6 ms,
a wheel step 8 to 11, motion, plain or captured, 8 to 9 with the longest
at 15 to 17 -- half a period and one. An input that wakes a window at rest
is drained by the frame drawn for it at once; a stream of them, by frames
paced to the display, each waiting on average half a period for the
drain.

By kind elsewhere (2026-10-31, `1d987ac`, the testbed with
`--frame-times`, one kind at a time, an input every 0.3 s, so each wakes
a window at rest; each figure given to the kind echoed before it). Under
wine on the thinkpad's bare Xorg, lavapipe at 60 Hz, uinput's hands, the
stamp wine's millisecond tick: a key 7.75 ms (80 of 80 stamped, longest
31.9), a middle button 7.59 (80 of 80, 26.3), a wheel step 1.24 (40 of
40, 7.5), motion 4.06 (42 of 43) -- the one unstamped the motion an entry
posts, 71 ms. A key's or a button's release comes 50 ms after its press,
while lavapipe still draws the press's frame, and waits for the next;
that is the 7 ms, where a wheel step alone is 1.2. On the Mac, Metal on
the HD 630, the agent's posted CGEvents, `NSEvent.timestamp` against
`systemUptime`: a key 1.78 ms (61 of 62 stamped), a wheel step 1.58 (23
of 23), a button 3.69 (39 of 39; the tail of 12 to 15 ms two presses in
one figure, a period apart), motion 3.02 -- half of it unstamped, the
agent's moves being a warp and a posted move of which one carries no
event of its own, read and not traced. Both clocks are the library's.
On aspire (the GTX 960M on NVK, 60 Hz, uinput, every input stamped, two
runs each), bare Xorg: a key 1.9 to 2.0 ms, a middle button 2.0, a wheel
step 1.4 to 1.7, motion 8.8 to 9.3; KMS by the copy path: a key 5.0 to
5.7, a button 4.3, a wheel step 2.7 to 3.0 (median 0.4, the drain with no
frame in the way), motion 8.7. Keys, buttons and steps a quarter second
apart are each drained by the frame drawn for it, which on the copy path
is a longer one; motion, a stream, waits half a period, as on weston and
the Pi. On this desktop (RADV, DP-1 at 60 Hz, the portal's hands, every
input stamped but the motion a Wayland enter posts): GNOME's Wayland a
key 0.85 ms, a button 1.56, a wheel step 0.59, motion 0.65; XWayland
2.18, 1.15, 0.43 and 2.29 -- mutter's and the X server's times on the
library's clock, each input drained by the frame drawn for it. On
aspire's Windows, both renderers on both GPUs: at rest 0.4 to 2.8 ms of
every kind, drawing at 60 Hz 7.5 to 9.5 ms -- half a period -- and the
renderers within 1.4 and 1.7 ms of each other in no one direction. The
drain is the frame loop's, which is the same under every renderer.

Whose clock the server stamps with, everywhere else, read off the rounds'
own `--frame-times` logs (2026-10-30 and 31, `build/drive/`): every input
of every kind the round's hands send counted, and how many came stamped.
Xorg on the bare machines, 507 of 507 and 509 of 509; XWayland on this
desktop's GNOME, 395 of 395 and 396 of 396; evdev under KMS, 740 of 741 to
744 of 744, on both kernel drivers; GNOME's Wayland, 233 of 242; weston,
278 to 285 of 304 to 307; wine on X11, Wayland and cage, 92 to 96 in a
hundred. So every server's clock is the library's, and what goes
unstamped is what the code says has no stamp of its own -- on Wayland the
motion an enter posts. A round's mean is no figure: it holds the inputs
around every resize, move and loss the round makes, and the longest is a
GPU move's.

Motion by kind, on the Raspberry Pi 4 (2026-10-31, `bench.py --hands`, the
testbed on Vulkan on the V3D, 20 measures of 30 moves each): on Xorg at 60
Hz 9.2 ms, 7.9 to 10.3; on KMS, its HDMI output forced on at 30 Hz, 16.5
ms, 15.2 to 17.5. Half a period and a millisecond on each, as on weston:
the wait for the next drain is the figure, wherever the stamp comes from.

**On Windows, raw input on a thread of its own** (`win32_rawin.c`). Raw
input carries no time, but a thread that does nothing else is woken by
each report as the system hands it over, so `now_seconds()` there is when
the report reached the process, to a fraction of a millisecond. The
window's thread, which pumps and dispatches everything else, reads the
message the report becomes later, and that wait is part of what is
measured. Each input message is matched to its report: a key by its
scancode, extended bit and direction, a button or the wheel by its
transition (by the device's button, which Windows swaps in the messages
and not the reports for a left-handed mouse), the one alike within two
tick steps of `GetMessageTime()`'s age (`TICK_SLACK`, 35 ms), and the same
key's before it spent. A `WM_MOUSEMOVE` takes every movement since the
last within that slack and counts each (`app_input_ages()`): the pump
waits in `GetMessage` and is woken by each move, so an older movement put
the pointer where this window was not -- another window, or the bar's drag
strip, which Windows answers with `WM_NCMOUSEMOVE`. A message with no
report keeps the tick's age; `--frame-times` says at close how many were
matched and how many were not (`raw input: 172 inputs matched to their
reports, 2 messages left to the tick`).

The process has one raw input registration a device, and the last made
wins, whoever made it; a removal is the process's too. So the thread
takes only a device the program has not registered for itself before the
window opened, gives back at close only a registration still its own,
and where it holds the raw mouse a captured pointer's motion, which was
read as `WM_INPUT` on the window, is handed from it (`FNL_WM_RAW`) and
drained ahead of every input message while captured, so a click lands
after the motion before it; where it does not, a capture registers on the
window as before. Only movement is handed on -- a button's report moves
nothing, and posted as motion of nought it would count as an input -- and
a report the queue has no room for is added to its newest, so the window
is handed the movement late and never short. The thread runs above
normal priority, since its time is the report's only while it is woken
at once. Not `RIDEV_INPUTSINK`: the reports reach the thread only while
one of the process's windows is in front, as the window's own did, so
nothing typed into another program reaches the library. A program that
registers raw input of its own after the window opened takes the device
from the thread, as it took it from the window before: its inputs then
keep the tick's age, and a capture, which asks the process who holds the
mouse, registers on the window as it did before the thread -- taking the
program's mouse for the capture, one registration a device being all a
process has, and giving it back at the release (`raw_unregister()`; at
the close under wine, whose wheel keeps it). Seen on aspire (2026-10-31,
a probe of the library's that registers both devices on a window of its
own, before `fnl_init()` and after): a key typed into another program in
front reaches no report and counts no input; registered before, the
library leaves both devices to the program; after, the program takes
them from the thread; and the capture works in both. Before the give-back
the program's mouse ended at the first capture, for the life of the
process. Not under
wine: its tick is a millisecond already, and its wheel
is read from raw input on the window (*Wine on the bare machine*), which
the thread would take.

Measured on schenker under Windows, the testbed on Vulkan on the GTX 1050
Ti, 60 Hz, driven by the agent's `SendInput` (which Windows hands to raw
input as it would a device's): 172 inputs matched, the 2 left to the tick
`WM_MOUSEMOVE`s Windows made with no report behind them; a key, a button
or a wheel step 0.2 to 0.6 ms once the window had drawn its first few, read
to the microsecond where the tick could say only 0 or 16; motion, plain or
captured, 7 to 10 ms with the longest at 14 to 17, as on weston. The
stress round there passed, capture included.

### Keys and focus

A key is delivered by physical **position**, not by what it prints. That is the
only thing evdev codes, Win32 scancodes and Cocoa virtual keycodes all convert
into without a keymap, and it is what a movement key wants: W is the forward key
on QWERTY and the same physical key on AZERTY, where it prints Z. Text entry
wants the opposite and gets it from `FNL_EVENT_TEXT`, which applies the layout:
see *Text entry*.

**The table is a keyboard, and every name in it is a place.** `FnlKey` is the
104-key board plus the key an ISO board has between left Shift and Z, each
named for what a US layout prints there: `FNL_KEY_Q` is the key right of Tab,
which an AZERTY board labels A. Media keys, F13 and up and the Japanese and
Korean extras arrive as `FNL_KEY_UNKNOWN`. Values are in the order the keys
were added, because the enum is append-only and a tidy order would have meant
renumbering the sixteen it began with.

On X11 a keycode is the evdev code plus 8 — the protocol reserves 0 to 7, and
the XKB `evdev` ruleset every Linux and BSD server has used since 2008 lays the
rest out in evdev order. So the two share one table,
`src/backend/evdev_keys.h`, and X11's translation is a subtraction in front of
it. The codes are written out there rather than taken from
`<linux/input-event-codes.h>`: on X11 they are the server's numbering, which
follows the kernel's by the ruleset's convention and is no part of it.

Win32's table is set-1 scancodes in two halves, `SC_PLAIN` and `SC_EXTENDED`,
because a message's extended bit is half the code, and three of its rows
are the backend's own doing. Pause and Num Lock arrive *swapped* in a message
relative to the wire, so the table is written the way the message spells them
and the focus-time seed skips both rather than ask `MapVirtualKey`, which
speaks the other dialect. The seed skips the keypad's digits and its Enter
too: they share virtual-key codes with the navigation cluster and the other
Enter, so a held Up arrow would read as a held keypad 8 that no release ever
clears. Print Screen is a release with no press, because the
press is the shell's. And where right Alt is AltGr, Windows sends a left Ctrl
press ahead of it. Driven under wine, the table holds for every key wine can
place; one it cannot — its scancode comes from matching the X keymap against
layouts it knows — arrives with a code no keyboard sends and is
`FNL_KEY_UNKNOWN`. Print Screen delivered nothing at all there, so that row is
the documented behaviour and **unverified**.

**Auto-repeat is delivered only to a text field.** Every binding this library
owns is a toggle, and a held F11 fullscreening thirty times a second would be
worse than useless. So a repeat is never a press: no binding acts on one, the
held set does not move, and with text entry off it is dropped. With it on it
reaches the caller as `FNL_EVENT_KEY` with `repeat` set. Where each comes from:
Win32 has them, marked by bit 30 of the message; X11 asks for XKB's detectable
auto-repeat, after which a repeat is a press with no release before it; and
Wayland sends none at all, so the backend makes them from
`wl_keyboard.repeat_info` with a timerfd in its epoll set. Where an X server
refuses the detectable flag, the release/press pair is caught by the timestamp
they share, which costs exactly one spurious release event per repeat interval
reaching the caller — no binding re-fires and the held set stays right.

**A key's text repeats when the system would repeat the key, whether or not
`FnlKey` names it.** A yen key is `FNL_KEY_UNKNOWN` and still types a
character a held key should type again. The `FNL_EVENT_KEY` repeat is the
name's: it goes for a key `app_key_repeats()` says repeats, never for the
modifiers and locks, and never for `FNL_KEY_UNKNOWN`, which says nothing a
caller could act on. Win32's characters and AppKit's come round with the
system's repeat by themselves, and the X server repeats by its own per-key
setting; Wayland times its own repeats and KMS's kernel repeats every key,
modifiers too, so those two ask the layout for a key `FnlKey` does not
name (`keymap_key_repeats()`): not where a group's first level is a
modifier, a lock or an ISO level or group key -- what xkeyboard-config's
compat marks `repeat= False` -- and otherwise yes.

`fnl_key_down()` answers for every key, one key at a time. `FNL_KEY_W` held
says nothing about `FNL_KEY_UP`, so a caller that binds both to one action asks
twice. The held set is the only record: there is no movement mask beside it
for the two to disagree.

The held-key set is maintained from both ends of focus. Focus leaving clears it,
since the releases that would have arrived go to whoever gets focus next; focus
arriving seeds it from the keys the server reports as already down. One-shot
bindings deliberately do not fire for a key that was already down when focus
arrived. Neither end posts an event — a key that was already down is state
rather than something that happened — so `app_key_seed()` and
`app_keys_clear()` ask for a frame themselves when the set moves; a held-key
readout would otherwise wait for something unrelated to redraw it.

Nothing but the titlebar can show which window has the keyboard — there is no
manager's caption to change colour — so it dims: bar, title and button icons.

### Text entry

`fnl_set_text_input()` switches it on, and while it is on a key press is
followed by `FNL_EVENT_TEXT` for each character it types — one code point an
event, as a number and as UTF-8. Held keys repeat, and `Escape` stops closing
the window, because it is what a text field cancels with. It is a mode rather
than a stream that is always there for those last two: both are right for a
field and wrong for a game, and only the caller knows which it is showing.

A flag first. All three effects are decisions the pump takes per key, read
from it, so the mode applies from the next key on — which is why the key that
opens a field does not also type into it. The change is forwarded as well
(`text_input_changed`), for what the backend holds: the compositor's input
method turned on or off on Wayland, a composition still up cancelled on
Win32, a held accent dropped elsewhere.

**Windows and macOS do the work and the other three do not.** `WM_CHAR`
arrives with the layout, the dead keys, AltGr and an input method's committed
string already applied; the backend's share is pairing surrogates. AppKit's
`insertText:` is the same. X11 and Wayland hand over *keysyms*, and KMS key
codes, and leave the layout to the client, and the libraries that normally
apply it are libX11 and libxkbcommon — neither of which is on the link line.
So `src/backend/keymap.c` is the part of them text entry needs, once for all
three:

- **Keysym to code point** is a table, `keysym_table.h`, generated from the
  comments in X11's `keysymdef.h` and tracked. Tracked because generating it at
  build time would make an X11 header a build dependency of the Wayland
  backend. It is two tables, and a translation unit gets one. The *values*
  are 16-bit `{sym, ucs}` pairs sorted by keysym and bisected, holding only
  what the number alone cannot answer: Latin-1 and Unicode keysyms are
  numbered as themselves, and the function block, where every Shift and arrow
  lives, prints nothing outside the keypad and is answered without looking —
  `tools/mkkeysyms.py` refuses to make a table those shortcuts are untrue of.
  The *names* are wanted only where a keymap arrives as text, so only
  `src/backend/keymap_xkb.c` includes them and an X11 build carries none:
  one pooled string, and rows of two 16-bit fields, an offset into it and
  the keysym. Offsets rather than `const char *`, because a pointer per row
  is a load-time relocation per row in the shared object. Every spelling
  the header has is a row, the ones xorgproto deprecated in 2023 included
  -- `masculine` for `ordmasculine`, `guillemotleft` for `guillemetleft`
  -- because xkeyboard-config's files still write those, and so does a
  compositor's keymap: the first sweep of the layouts (below, *KMS*) read
  German's `»` and `«` as empty levels on their account. The table is made
  from the newest `keysymdef.h` to hand (xorgproto 2026.1, Arch's), which
  names `SSHARP` and `rightsingleanglequotemark` where Debian's 2025.1
  does not; a name the header lacks reads as nothing, so the newer header
  is the one to generate from.
- **Which level** is the X11 core protocol's rule plus the two things XKB added
  that a real layout cannot be typed without: a second pair of levels behind
  AltGr, and groups. A key's XKB *type* is read for its level count and,
  for a group key, the modifier that selects its second level -- nothing
  else. The four levels are
  taken to be plain, Shift, AltGr and AltGr+Shift; Caps Lock inverts Shift for
  a pair that is one letter in two cases and touches nothing else; Num Lock
  picks the keypad's digit.
- **The keymap itself** comes three ways. X11's is `GetKeyboardMapping` and
  `GetModifierMapping` from the core protocol — no XKB map request, which
  would be a good deal more hand-encoded protocol than the one flag already
  here. An XKB server keeps that core table in step with the layout, two groups
  of four levels in a fixed column order, and puts the group in bits 13 and 14
  of each key event. Wayland's is the *text* of an XKB keymap, read by a small
  tokeniser that takes `xkb_keycodes` and `xkb_symbols` and skips everything
  else by counting brackets. Both are read again when the server says the
  keymap changed. KMS's is nobody's: the console's names are resolved
  through xkeyboard-config's rules and its symbols files read by the same
  tokeniser, following their includes (*KMS: no window server*).

**Composing is two mechanisms, and which one runs depends on the desktop.**
Some text takes more than one key: an accent and then its letter, or whatever
an input method turns a run of keys into. What has been typed towards it and
is not yet text is the *composition*. It arrives as `FNL_EVENT_COMPOSE`, is
read with `fnl_compose_text()`, and ends as ordinary `FNL_EVENT_TEXT`.

- **On Wayland, the compositor's input method**, through `zwp_text_input_v3`:
  dead keys, Compose sequences and CJK entry, with the composition reported as
  it grows. It is enabled only while text entry is on *and* the protocol says
  the surface has its focus — left on for a game it would be composing the
  movement keys. It does not replace the key path and the two never overlap:
  a key the input method has no use for arrives as `wl_keyboard.key` and is
  typed through the layout as before; one it takes is never delivered as a key
  at all. So nothing is typed twice and nothing is reconciled. Its *release*
  may still be delivered, which is the one oddity a caller can see.
  `fnl_set_text_rect_pt()` says where the caret is, for the candidate list.
- **Otherwise, the library's own composer**, for dead keys only. An accent key
  is held in a `KeyCompose` and joined to the letter that follows; a space, or
  the accent again, types the accent itself; a letter it does not go on
  arrives bare. The table is generated from Unicode's canonical
  compositions by `tools/mkkeysyms.py` — not from a Compose file, which is a
  list per locale with no derivation. This is all of composing on X11 and
  KMS, and on Wayland it is the fallback for a compositor with no input method.
  `FNL_NO_INPUT_METHOD=1` in the environment makes any compositor one of
  those: the protocol is left unbound, and text is typed through the layout
  and this composer. A diagnostic, read where the global is offered.
- **On Win32 and macOS the system composes** and the result arrives as
  text. Its composition in progress is reported as `FNL_EVENT_COMPOSE`, for
  the caller to draw (below).

**X11 only hears that the keymap changed if it asks XKB.** The backend calls
`XkbUseExtension` for detectable auto-repeat, and from that moment the server
stops sending it the core protocol's `MappingNotify`: an XKB client is expected
to select XKB's own events, and without them the layout read at startup is the
layout for the life of the window. So the backend selects (`xkb_ask()` in
`x11_ext.c`) `NewKeyboardNotify` and `MapNotify`, hand-encoded like the rest —
and `MapNotify` is selected by `affectMap` and `map`, not by `selectAll`, which
it ignores: with those at zero the request succeeds and no event is ever sent.
The events only mark the layout stale; it is read again by the next key that
types, because a layout change arrives as a burst of them and each read is a
round trip on the thread that pumps input — one, not two: both requests are sent
before either reply is collected.

What that leaves out, stated rather than hidden:

- **No Compose key and no input method on X11.** Dead keys are composed; the
  multi-key sequences are not, and there is no XIM.
- **X11 reaches two layouts, not four.** The core table has columns for two
  groups; a key typed under a third falls back to the first.
- **Which bit is AltGr or Num Lock on Wayland is a convention**, `Mod5` and
  `Mod2`, which is xkeyboard-config's and is what every desktop's layouts are
  written to. X11 finds both by looking for the keysym among the modifier keys.
- **Caps Lock knows Latin, Greek and Cyrillic**, Turkish's two i's included.
  A script outside those types correctly and ignores it.

**The rules are checked on every build**, by `tools/verifykeymap`: the layout
code against a keymap written to have every case in it — the four levels,
Caps and Num Lock, groups, a level left empty, every dead-key outcome — and
then its parser against that keymap truncated at each byte and damaged four
thousand ways. It is the in-build check `verifyfont` is for the atlas, and
for the same reason: no round can ask these from outside, since a round types
on the desk it is given. Three of its cases are rules a round cannot reach: a
keysym *name* with no row in the table reads as an empty level on Wayland, and
a key whose first level is empty is taken to have none — so the keypad needs
`KP_Home` and its neighbours named or it types nothing; BackSpace and Escape
take a held accent back, which needs their rows too; and a key with an empty
level under a full one still prints with Shift.

**What has been driven.** `stress.py` types in all three environments under
any layout: one character a key, Shift for its other case, nothing under Ctrl,
repeats while held, Escape left to the caller. The input method is driven on
Wayland through IBus's Unicode entry (`Ctrl+Shift+U`, hex, Space), which needs
no particular layout. The library's composer is driven on X11 by putting
`dead_acute` on a spare keycode for the length of the check. Windows' is
driven in both guests by switching the window itself to a layout that has a
dead key: the agent loads United States-International, which no Windows has
to have installed, and posts the window `WM_INPUTLANGCHANGEREQUEST`, as the
language bar would. The window follows it from another process and follows
it back — the key right of the semicolon typed `ä` under the layout the
guests start with, nothing after the switch, and `ä` again — and what arrives
is the layout's rules and not the library's: `é` after the accent and `ü`
after Shift's, but the accent and a space type U+0027 and never U+00B4, and
a letter it does not go on arrives *behind* the accent, U+0027 U+0078, where
the library's composer would have typed it bare. No composition is reported
between the two keys.

A real dead key from a real layout is driven wherever the window types
through this desktop's: the round switches GNOME's input source to German
with its dead keys under the running window (`desk-layout`), types, and
switches back. On Wayland that is the compositor's input method, on X11 the
library's composer, under wine on X11 wine's: all three type nothing for the
accent, `é` after it and `ù` after Shift's, and the key is `´` again once
the layout is back. Where they differ is theirs to — the accent and a space
are U+0027 through the input method and under wine and U+00B4 from the
library — so the check takes either. A dead key made by editing the X
keymap under wine was tried first and is no test of anything: the accent and
the letter arrived apart, U+00B4 U+0065, and keys after it came without
their presses.

The composer as Wayland's fallback is typed through in a window as well:
GNOME always has an input method, so the round is started with it left
unbound, `FNL_NO_INPUT_METHOD=1 tools/drive/stress.py wayland-vulkan`. The
desk's dead key is then held by the library, `composing ['´']`, the letter
after it arrives accented, and an accent and a space type U+00B4 where the
input method types an apostrophe -- which is the check that says whose
composer answered. 50 checks, none failed, against the 53 with it bound:
the input method's own section is the one skipped.

**On Win32 the composition is the caller's to draw**, as on Wayland and
macOS: `WM_IME_SETCONTEXT` is answered without
`ISC_SHOWUICOMPOSITIONWINDOW`, `WM_IME_STARTCOMPOSITION` is the window's,
and `WM_IME_COMPOSITION` types what was committed and reports what is left
as `FNL_EVENT_COMPOSE`, in Wayland's order, and is not passed on -- so the
commit is never typed again as `WM_IME_CHAR`. Text entry switched off
cancels a composition still up. The caret (`fnl_set_text_rect_pt()`) is
handed over in client pixels each time it moves and again at
`WM_IME_STARTCOMPOSITION` (`imm_place()`): the candidate list is told to
keep off it (`ImmSetCandidateWindow`, `CFS_EXCLUDE`), and the composition
window, never shown, is put at it as well (`ImmSetCompositionWindow`),
since an IME may place its candidates by that one. `imm32.dll` is opened
at run time (`imm_load()`), off the link line. Left to `DefWindowProc`,
wine on X11 showed a dead key's accent in a box of its own: an
override-redirect X window of the program's, 25x36, white with the accent
in it, at the window's bottom-left, mapped while the accent was held -- a
watcher on the X server saw it map and unmap at each of the round's dead
keys.

**Not driven:** an input method's composition on Windows itself, where a
dead key is the layout's and composes nothing, and so where its candidate
list lands; and any of it under wine on Wayland, which does not type
through the desk's layout at all (see tools/drive/README.md *Driving a
window*).

### The clipboard

Plain text as UTF-8, and nothing else: no images, no rich text, no drag and
drop. `fnl_clipboard_set()` offers a string. Reading is a request and an
answer — `fnl_clipboard_request()`, then `FNL_EVENT_CLIPBOARD`, then
`fnl_clipboard_text()` — because on Wayland and X11 the text is in another
program and has to be asked for. **A request is always answered**,
with a length of zero when there is no text; a path that forgot to would be a
paste that hangs.

**The other program is given three seconds, on Wayland and X11.** It may
hang, or die between two pieces, and neither sends anything; each piece that
arrives renews the wait and silence ends it. An answer cut short that way —
or past 64 MiB, or not text — is reported as an empty clipboard and not as
the part that arrived, which may end inside a character and is not what was
copied. Requests made while one is in flight share its answer. The assembled
paste is handed to the core as it is (`app_clipboard_take()`), not copied a
second time.

Neither the Wayland nor the X11 backend holds the bytes it offers. Offering is
a promise to hand them over when asked, which may be never or many times, so
the string lives in `App::clip_out` — the pump's alone from the moment it takes
it — for as long as the offer stands.

- **Wayland** is `wl_data_device`'s selection. A paste arrives down a pipe at
  the owner's pace, so its read end joins the epoll set and is read one chunk a
  turn until end of file; the pump never loops on another program's
  descriptor. This window's own selection is answered from the string
  directly: the long way round would have the compositor ask this pump to
  write to a pipe this pump is waiting to read. Which selection *is* this
  window's is read off the offer and not remembered from having asked: the
  source lists one mime type nobody else does, and an offer carrying it is
  ours come back. A compositor may drop `set_selection` without a word, and a
  window that believed its own request would then paste its own string over
  whatever was really copied. A request made after this window copied is
  answered from the string even while an earlier paste is still arriving:
  that one is dropped unanswered, since requests share one answer and the
  other owner's text is no answer to the later one. Offering writes with
  `SIGPIPE` blocked, since a reader that asked and then died would otherwise
  take this process with it, and never blocks: the reader is another program
  and a pipe holds 64 KiB, so one that asked and stopped reading is given a
  quarter of a second for the whole string and then left with what it took,
  rather than holding the pump. A client may set the selection only on the
  strength of an input event, so both calls belong in answer to a key press.
- **X11** is ICCCM's `CLIPBOARD` selection on the core protocol. Asking
  implements the receiving half of `INCR`, because a browser sends anything past
  a few hundred kilobytes that way and a paste that fails on a long page is not
  a paste. Offering refuses a string too large for one request rather than
  implement the sending half, which is a state machine per asker. An owner given
  up on at the deadline may still answer, so a request is stamped with the
  server's time -- read off a zero-length write to a property of the window's
  own, since ICCCM wants a time and not `CurrentTime`, and an owner may refuse
  one older than its ownership -- and an answer carrying another time, or
  arriving on the other property, is a late one and ignored; the request after
  one given up is asked on a second property, where the late owner's INCR pieces
  land unread. A request made after this window copied, while an earlier paste
  is still on its way, gives that paste up and asks again, of this window now
  the owner, so the one answer is what was copied, as on Wayland; a paste with
  no request after the copy keeps the answer it asked for. Ownership is taken at
  a server time too, read off a stamp of its own, since no event of the copy's
  reaches the backend to carry one. The stamps come back in the order written,
  so a copy and a paste in one turn take the selection before they ask for it,
  and only the latest of each acts: a copy made before the last one's stamp came
  back supersedes it, and a paste asked again waits for its new stamp. They
  are paired by counting, so a stamp that could never come back -- the
  property's atom not interned -- is not written at all: the copy and the
  paste then act at once, at `CurrentTime`, rather than leave every stamp
  after it one behind and every paste to its deadline.
- **Win32** is `CF_UNICODETEXT`, synchronous: the clipboard there holds the
  bytes and not a promise of them. It is also one lock for the session, so
  opening it is tried a few times; and what is read is bounded by the block's
  own size rather than by a terminator another program was trusted to write.
  Its lines end in CRLF, which its edit controls want and its programs put,
  and the caller's in LF on every system (`fenestal.h`): an LF without its
  CR gains one on the way out and a CRLF loses the CR on the way in. A lone
  CR passes either way, as the other backends pass the owner's text, which
  there is LF by convention and is not rewritten.

Driven against `xclip` as the other program, in both directions in all three
environments, including a 640 kB paste — past one X property and past one pipe
buffer, so the incremental paths are what carry it.

### The pointer's shape

`fnl_set_cursor()` chooses the pointer's shape over what the caller draws: the
presentable area, and a titlebar or a run of one that is the caller's. It is
state rather than a region — it stays until the next call — because a region
would be the library holding a caller's layout, rebuilt every frame the way
`fnl_titlebar_claim()` rectangles are, for a question the caller already
answers each time `FNL_EVENT_MOTION` arrives.

**The library keeps three places.** Its own titlebar and controls show the
arrow, a resize edge shows its direction, and a captured pointer shows nothing.
`deco_cursor_is_callers()` is the one definition of where the caller's shape
applies, and `deco_cursor()` in `app.h` is the one precedence — capture, edge,
caller, arrow — whose answer each backend maps through its own tables.
Wayland, X11 and KMS use all of it, in a `cursor_update()` each (KMS's window
is the output, so its edges draw the arrow), and Cocoa's `cursor_apply()`
leaves hiding to the capture's counter. Win32 takes only the last
two from it (`cursor_apply()`): its edges are answered from `WM_NCHITTEST` and
drawn by Windows, and a capture hides the cursor through `ShowCursor`'s
count.

**Nine shapes, and the bound is X11.** Wayland asks the compositor for a named
shape and Win32 loads the system's, so either could offer dozens. X11's come
from the core cursor font, which has corners and single-axis double arrows but
no diagonal double arrow. The list is what every backend can draw without a
library, and `FnlCursor` is append-only so that it can grow when that changes.

**There is no hidden shape.** Hiding the cursor is what a capture does, and a
pointer that is hidden but still free is one the user cannot find.

The call is `[any]`-thread and forwarded to the pump like the other knobs, and
it compares before it records: setting the shape every frame from `on_overlay`
is the natural thing to write, and each forwarded request is a wakeup of a pump
with nothing to do. On Wayland the change needs `wp_cursor_shape_v1`; without
it the cursor stays as the compositor left it. The Win32 half is
`WM_SETCURSOR` for the client area, with the edges left to `DefWindowProc`.

## The frame

### Frame pacing and the off-screen skip

FIFO throttles a present against the swapchain's own presentation history, so
the first present after a rebuild is never throttled. A drag-resize rebuilds on
every configure, which makes *every* present a first present and removes the
throttle entirely: left alone the loop rebuilds and commits several hundred
times a second.

On Wayland the render thread paces on `wl_surface.frame` instead, which belongs
to the surface and survives a rebuild. Every vsynced frame waits for the
invitation, not just the rebuilt ones — pacing only rebuilds leaves the frames
between them blocking in the FIFO acquire, which costs a resize step two
refreshes instead of one. In a tearing mode only the rebuild is paced.

An invitation that never arrives is waited out for `FRAME_WAIT_S` and then the
frame is **skipped** rather than drawn anyway: a compositor stops inviting
because the window is minimized, covered or on another workspace, so a frame
drawn then is one nobody can see. Minimized, that is 0.0% of a core against 2.3%
visible at 60 Hz, and the FPS counter reads correctly the moment the window
comes back rather than climbing out of a hole.

Three things are still drawn uninvited. An unanswered configure, a resize or a
state change, since those are answers the window server is owed either way.
Anything at all until the first invitation ever arrives — a compositor that
never sends them must not leave the window blank. And the moment right after a
state change was answered, for `TRANSITION_GRACE` seconds: leaving fullscreen,
mutter goes quiet for about 110 ms while it takes the window off the scanout
plane, and reading that silence as "nobody is watching" costs the transition
another timeout on top of it.

A rebuild answering something **urgent** is never paced: a change of toplevel
state. The compositor holds its own frame, and with it
the next invitation, until it has a buffer at the size that state implies — so
waiting for an invitation there is waiting on something that is waiting on you.
Both are discrete events rather than the stream a drag-resize produces, which is
exactly why the same exemption cannot be extended to resize configures without
losing the throttle.

**Every backend but Wayland has no invitation to borrow, so the loop paces
itself**: a
turn is held until a display period has passed since the previous one. Nothing
is given up for it. At most one frame a period reaches the screen, so a frame
that would have been the second in a period was going to be dropped whole; what
the wait collects instead is the configures that land during it, and the second
look answers with the newest size. It is the argument late latching is built
on, applied to a size rather than to input.

Which turns are spaced, and what the spacing is measured from, are two rules.

**Every vsynced turn is spaced, not only the rebuilding ones.** The `rebuild`
flag is read at the first look, and a turn becomes a rebuild at the *second*
one whenever a configure lands during the hold — which during a drag is most of
them. Gating on the flag leaves every one of those unpaced: at 235 configures a
second it takes the rebuild rate from 235 to about 190 rather than to 60. It is
the rule Wayland keeps by waiting on `wl_surface.frame` for every vsynced
frame; in a tearing mode only a rebuild is spaced, since there the user has
asked for frames as fast as they can be made. An urgent frame is never spaced,
exactly as it is never paced.

**The spacing is measured from the previous turn, not from the last rebuild.**
The anchor is the previous vsynced turn for as long as rebuilds keep arriving
(`PACE_HOLDOVER`, half a second), and the last rebuild — stale by then — once
they stop, which is what hands an idle window back to the FIFO throttle rather
than pacing it twice. Anchoring on the rebuild throughout is the alternative,
and it is wrong: a rebuilt swapchain has no presentation history, so the queue
stays refillable for as many frames as it has images — the same fact
`LATE_SLACK_SETTLE` exists for, read as a cost rather than as noise in a
measurement. The spacing lets go one period after the rebuild, the acquire does
not block because the queue is still not full, and the only thing left pacing
the loop is the late-latch hold — which is a *fraction* of the period by
construction. The loop then settles at period/hold, which is half again the
display rate. That needs a rebuild rate low enough to leave turns between the
rebuilds and high enough that the queue never fills, which is everything from a
couple a second up to the display rate: every rate an interactive drag passes
through. Both anchors are in the tables below.

**A held turn is anchored where its hold was due, not where it came back.**
No timed wait returns early, so an anchor read after the hold spaces every
turn by the period *plus the wait's lateness*, and a rebuild dated the same way
adds it again. Under wine the hold comes back 0.34–0.63 ms late (the mean of
each second's sixty), which is two frames a second: 57–58 fps at 15 to 45
sizes a second where X11 lost one at most. So `pace_at` is the deadline when
the hold overran it by less than `PACE_LATE_MAX` (2 ms — more is a stall, and
the next turn is spaced from where it ended), and a rebuild made by a paced
turn dates from that turn's anchor rather than from its own clock read
(`App::pace_ran`). Which turn made the rebuild is asked, not inferred: dating
every rebuild that fell within a period of the last paced turn from that turn
was tried, and read 62–63 fps on X11 at 45 and 60 sizes a second. With both
rules, three sweeps of the table below read 60 on X11 in twenty of twenty-one
cells, and 59–62 under wine around 60 — what is left there is the period
estimate, which wine holds to 16.62–16.80 ms rather than X11's 16.63–16.73,
and a hold cut short, which is meant to be early. `--frame-times` prints the
estimate as `period`. On Windows itself it is no looser than under wine:
at 30 and 60 sizes a second, 16.53–16.69 ms on schenker and 16.64–16.76 on
aspire, with one cell there reaching 17.27, against wine's 16.64–18.82 in
the same sweep. X11 also has the refresh RandR reports, and spaces by the
larger of that and its estimate (below).

Safe in both directions, which is what makes it safe to leave on. An estimate
that runs long keeps the queue draining, so the acquire goes on not blocking and
the loop simply runs at the estimate. One that runs short lets the queue fill,
and the acquire re-establishes the true period itself.

A turn the spacing held is then excluded from the period estimate below, and
has to be: the hold is skipped on it and the acquire finds an image free at
once, so the figures sum to a few milliseconds rather than to a period.
Fed that, the estimate collapses towards it and takes the spacing with it —
measured at 13.4 ms against a true 16.68, and a window running at 70 fps
because that is what it is being spaced to.

**The period is measured** rather than given, out of figures the turn
already holds: what it held, what it waited on the device's fences, what its
present blocked for, what the frame itself cost, and on X11 what it waited
for the server to show the frame before. On a throttled turn
those partition the period — 11.29 + 0 + 5.27 + 0.07 against a 16.67 ms
frame. Sampled only where each means what it says: not on a rebuild turn
nor in the `LATE_SLACK_SETTLE` frames after one, where the present is not
throttled and the sum is short; not on a turn the spacing itself held, for
the reason above; not in a tearing mode, where nothing blocks; and not
before the window has settled. A drag therefore samples nothing and reads
the estimate it settled on beforehand, which is the intent.
Nothing is spaced until an estimate exists, unless the window server has
said what a refresh is; see below.

**Before the window has settled the spacing borrows a period.** A drag begun
in a window's first second had no estimate to read and, sampling nothing,
got none for as long as it lasted: on the first laptop's NVIDIA device, which
blocks nowhere, a border held from the first second was 93 fps on a 60 Hz
panel and 62% of a core until the hand let go -- `frame 8.36 ms`, `pace
0.00`. So until the sixtieth present the spacing takes the mean of four
samples in a row that agree to a tenth, each between 2 and 50 ms
(`disp_period_early()`): a window still being mapped gives no four alike,
and a swapchain not yet throttled gives four under the floor. The first
sample of the settled window replaces it whole and is not averaged in,
because a borrowed period that was short would drop every true one as a
missed cycle and stand for good. Only the spacing and X11's display hold
read it. The late-latch
controllers still wait for the settled window: a hold from a wrong period
is a late frame, where spacing by one is safe in both directions. With it
the same drag is 50 fps and 36% of a core, `pace 6.38`, which is what a
rested window gives; the second laptop's NVIDIA device is 57 either way,
and the Intel devices, whose rebuild is longer than a period, are where
they were at 29 and 34. Lowering the sixty for every estimator is the
alternative and was not tried: it guards the late-latch delay as well. On
the build with it: the bare round, Wayland 53, X11 74, wine 65, wine on
Wayland 38, the two guests and the first laptop 66 each and the second 63;
`minimal.py` on all eight, 19 each; AddressSanitizer and ThreadSanitizer on
the two native targets, 53 and 74; and a sweep in which the four local
environments read 59 to 62 in every cell and the laptops what they did. None
failed.

**And never by less than one refresh, where the window server says what
that is.** Every period above is *measured*, from what blocked, and a driver
that blocks on nothing makes the frame's own cost read as a period. On a bare
X server with lavapipe that was the whole loop: its software swapchain holds
no present to the display while it resizes, and with MIT-SHM off not even at
rest, so the estimate settled on the 7 ms a frame cost and spaced the window
to 140 fps on a 60 Hz panel -- 149 at 45 sizes a second and 2.4 cores, 152 at
rest with `noshm`. So the X11 backend reads each monitor's refresh off its
CRTC's mode (`x11_refresh_read()`, two RandR round trips on the pump's turn
after the monitor list, so opening waits no longer) and publishes the one
the window is on (`app_publish_refresh()`). `pace_period()` then spaces by
the larger of the two, and a measured period under `PACE_UNTHROTTLED`, 0.6,
of the refresh is no period at all: the turn is spaced by the refresh, and from
the turn before even with no rebuild about, since FIFO is not holding the
loop between rebuilds either. Where FIFO does throttle the estimate already
sits at the refresh and nothing moves -- this desktop's X11 column reads 60 in
every cell as before, at the CPU it had. A backend that reports no refresh
(a screen that names no rate, a server without RandR) has the measured period
alone.

| sizes a second | before | after, MIT-SHM | after, `noshm` |
|---|---|---|---|
| idle | 60 fps, 54% | 60, 54% | 60, 65% |
| 5 | 76, 66% | 60, 56% | 60, 57% |
| 15 | 105, 123% | 60, 94% | 60, 79% |
| 30 | 139, 207% | 60, 123% | 60, 98% |
| 45 | 149, 245% | 60, 132% | 60, 106% |
| 60 | 126, 233% | 60, 140% | 60, 113% |
| 250 | hung, see *X11* | -- | 60, 114% |

The thinkpad, `x11-vulkan-bare`, a 1366x768 panel at 60.01 Hz. The border
row there was 32 fps at three cores either way: a drag of the testbed's corner
is lavapipe drawing the testbed's scene at every size, and the spacing has
nothing to take off that. What did was the scene's own arithmetic (*X11*,
*On lavapipe a frame is its pixels*): under `noshm`, 45 fps at 283%.

**The fence wait is part of the period, though it is no spare time.** It was
left out, as it is left out of the late-latch signal, on the reasoning that a
device behind should collapse the estimate and so switch the spacing off. But
where a FIFO swapchain is held to the display is the driver's choice, and
NVIDIA's on a hybrid laptop — a GTX 1050 Ti presenting through the Intel
device's panel, 582.78 — holds it in the fence: `wait 16.38`, `present 0.11`,
`gpu idle 0/31` on an empty scene at 60 fps. The sum there was the frame's
cost alone, a fifth of a millisecond; no period was learnt, nothing was spaced,
and the window ran at 72, 104 and 95 fps at 5, 15 and 30 sizes a second, which
is the "without" column of the table below on a machine that had the spacing.
With the wait counted (`App::device_block`) the same cells are 60, 55 and 56.
A device that really is behind now reads as a longer period, and spacing by
that is spacing by the rate the loop runs at anyway. The late-latch hold still
leaves the wait out, and on that device still settles at zero: there is no
block to spend.

**A sample half again the estimate is a missed cycle and is dropped**, which is
the rule `LATE_MISS_FACTOR` states and every period estimator in
`src/core/frame.c` keeps. The sum cannot tell a frame that missed its cycle from
a longer period — the hold and the block simply add up to twice as much — and
this desktop drops 0.3–0.6% of cycles by itself, each of which would fold a 33
ms sample in at 5%. Without the guard the estimate sits anywhere between 16.65
and 17.10 ms against a true 16.68, and spacing against a period 4% long is 57
fps on a 60 Hz display. With it the estimate holds 16.63–16.73.

What it is worth, on X11 against a 59.95 Hz display, driving a 1280×720 testbed
window at a fixed configure rate: with no spacing at all, with the spacing
anchored on the rebuild, and as it is.

| configures/s | without | rebuild-anchored | turn-anchored |
|---|---|---|---|
| none (idle) | 60 fps, 2.1% of a core | unchanged | unchanged |
| 15 | 74 fps, 12% | 80 fps, 11.3% | **60 fps, 8.5%** |
| 60 | 117 fps, 16% | 82 fps, 14.8% | 60 fps, 13.8% |
| 120 | 115 fps, 22% | 60 fps, 14.3% | 60 fps, 14.2% |
| 235 | 235 fps, 43% | 60 fps, 14.0% | 60 fps, 14.0% |

And across all three environments, driven identically — a caller asking for
its own sizes from a thread of its own, an empty scene, frames presented and
per cent of one core. Each X11 and Win32 cell is rebuild-anchored →
turn-anchored:

| sizes/s | X11 | Wayland | Win32 |
|---|---|---|---|
| idle | 60 → 60, 2.3 → 2.5% | 60, 2.5% | 60 → 60, 2.7 → 2.8% |
| 5 | 70 → 59, 6.5 → 4.7% | 60, 3.1% | 71 → 60, 8.2 → 6.2% |
| 15 | 83 → 59, 12.6 → 8.5% | 60, 4.5% | 82 → 60, 12.5 → 12.0% |
| 30 | 71 → 59, 11.8% | 60, 6.3% | 75 → 60, 14.0 → 14.3% |
| 45 | 90 → 60, 13.5 → 14.0% | 60, 7.5% | 90 → 60, 17.1 → 17.3% |
| 60 | 60 → 60, 15.1 → 14.8% | 60, 8.6% | 62 → 59, 20.0 → 19.8% |
| 250 | 60 → 60, 15.5 → 14.6% | 60, 10.2% | 57 → 60, 32.3 → 32.6% |

The same sweep with wine's Wayland driver beside the other three, one run,
467 s:

| sizes/s | Wayland | X11 | wine on X11 | wine on Wayland |
|---|---|---|---|---|
| idle | 60 fps, 2.2% | 60, 2.2% | 60, 2.7% | 60, 2.5% |
| 5 | 60, 2.7% | 59, 3.8% | 60, 5.8% | 60, 3.8% |
| 15 | 60, 4.0% | 60, 8.8% | 60, 10.7% | 60, 6.2% |
| 30 | 60, 5.5% | 60, 9.3% | 60, 12.8% | 60, 10.0% |
| 45 | 60, 7.2% | 60, 10.8% | 60, 16.5% | 60, 12.7% |
| 60 | 60, 6.7% | 60, 13.2% | 60, 21.0% | 60, 16.5% |
| 250 | 60, 10.5% | 60, 14.5% | 60, 38.3% | 62, 32.5% |

Wine costs what it cost through either driver, less through the Wayland one
at every rate that resizes. The loop's per-second frame time in those logs,
at 60 and at 250 sizes a second, is 16.57–16.66 and 16.18–16.70 ms under
wine on X11 and 16.51–16.67 and 15.68–16.64 ms under wine on Wayland,
against X11's 16.72–16.78 and 16.66–16.69: the looseness is wine's on either
driver and not XWayland's. That is the frame as it ran and not the period
estimate itself, which nothing prints.

Wayland is the control and is flat at 60 throughout: it waits on
`wl_surface.frame` for every vsynced frame and has no anchor to get wrong. That
is what says the rule is the anchor rather than anything about swapchains.
Win32's cost at 250 sizes a second is `SetWindowPos` re-entering the window
procedure once per *request*, which is the caller's rate rather than the
display's — see *Win32* below.

A rebuild costs 4.0 ms on X11, of which 3.6 is inside `vkCreateSwapchainKHR` and
0.07 is the depth attachment, with 0.02 for the views, semaphores and command
buffers and nothing measurable for the framebuffers or the hand-over to the
retired list — timers inside `recreate_swapchain()`, the mean of a hundred
rebuilds, `fenestal-pace` asking for 30 sizes a second around 1000x654. So there
is no cheaper rebuild to write and the count is the only figure that can come
down. The same build on Wayland is 0.47 ms, 0.39 of it the driver's call: what
X11 pays for is the server's side of a new swapchain, which is one more reason
the spacing matters most where nothing invites. At 60 configures a second it
cannot: sixty configures need sixty rebuilds, and what the spacing removes there
is the second present in each period rather than a rebuild. Above the display
rate it removes the rebuilds too, which is where the 43% goes. Win32 is the case
that needs it most rather than the same case: `WM_SIZE` comes out of the modal
resize loop at pointer-message rate, which is not the display's.

Every backend but Wayland has no invitation, so the skip is split across both
seams.
`FnlBackendVtbl::window_on_screen` is the window server's half — minimized or
hidden, which Win32 answers from `IsIconic` and `IsWindowVisible`, and which
Wayland declines to answer because not inviting says it more precisely.
`FnlRendererVtbl::occluded` is the graphics API's half — *covered*, which no
window server reports. Either saying no is enough. `backend_frame_wait_shown()`,
the frame wait those backends name, has to *wait* out `FRAME_WAIT_S` rather than
return false at once: the loop answers a skip by going straight round again, so
a bare `return false` would spin on the visibility test instead of on the
present.

`vk_occluded()` answers false, which is the honest answer for a renderer with no
such report — Vulkan has nothing like `DXGI_STATUS_OCCLUDED` anywhere in WSI, so
it does not know and must not claim the window has gone. The one exception is
the Mac, where AppKit's occlusion state is the window server's own report and
MoltenVK's layer is Metal's: the bridge reads it there (`VkBridge::occluded`),
as Metal's and OpenGL's do. On Wayland that costs
nothing. On Win32 under Vulkan a *covered* window goes on rendering while a
*minimized* one is parked by the backend half: a stated limitation rather than
a silent one. Direct3D 12 does report it, and the loop parks on that (*A
window nobody can see*).

X11's pump blocks on the connection's descriptor and the wakeup eventfd exactly
as Wayland's does, and — like Wayland's — the wait is indefinite. That costs a
**second X connection**, and the two are the same decision.

An epoll set is a claim that a socket with nothing to read means there is
nothing to do, and that claim only holds where the waiting thread is the sole
reader. Mesa's WSI *blocks* in whatever connection it is handed —
`xcb_wait_for_special_event`, once a frame in FIFO — and every ordinary event it
reads on the way to its own goes into xcb's decoded queue rather than staying in
the socket. Share one connection and there is a window, between the queued pass
finding nothing and `epoll_wait` blocking, in which an event taken by the other
reader leaves the socket empty and the queue full. Bounding the wait bounds that
but does not remove it, and what it costs is the last event of a gesture held
for the length of the bound: a pointer stopping short of where it was put down
and catching up a tenth of a second later.

So `BackendState::conn` is the window's and only the pump reads it, and
`BackendState::gfx` is handed to `VK_KHR_xcb_surface` and belongs to the
presentation engine. The window is created on the first and presented into
through the second, which needs no arrangement: an X resource id belongs to the
server, not to the client that asked for it. The cost is one descriptor, one
`xcb_connect` at startup, and one rule — a *reader* on either side must not
cross over. A write may, with one duty: libxcb polls the socket for input
while it waits to send, and reads whatever has arrived into the decoded queue,
which opens the same window from the writing side. So whoever writes on
`conn` off the pump wakes the pump afterwards, and its turn drains the queue:
the sync counter's value is the only such write, set and flushed by
`x11_flush()` on the render thread and `x11_ack_release()` on the renderer's
waiter, and queued by `x11_frame_cancel()` for the woken pump to send. The
pump's own flush comes ahead of its queued pass for the same reason, never
between that pass and `epoll_wait`.

Two smaller consequences, both in `x11.c`. The graphics connection's ordinary
event queue has no reader of its own, so the pump drains it (`gfx_drain`) with
`xcb_poll_for_queued_event` — the form that cannot touch the socket. What lands
there is errors, worth a line each, and Present events orphaned when a swapchain
rebuild unregistered the special queue they were bound for; once per pump turn
is enough for the second, since a rebuild answers a resize or a state change,
which is an event, which is what woke the pump. And `x11_close()` disconnects
the graphics half *first*, before the window it presents into is destroyed.

### At rest

**A frame that would show what the last one showed is not drawn.** The
recording cache already knew when that was -- the draw state compared equal,
no event was queued, `fnl_invalidate()` had not been called -- and presented
the command buffer it had, every refresh. On a GPU that cost 2-3% of a core.
On a software device it is the whole cost: the ThinkPad's lavapipe,
`fenestal-pace` at rest under `noshm`, spent 50% of a core rasterising the
same image across its four threads and the X server 31% taking each one in.
So `rest_owed()` (`src/core/frame.c`) asks, after the state is latched and
before anything is acquired, whether there is a change -- a rebuild, a
configure to acknowledge, something urgent, a recording no submitted frame
has drawn -- and a turn with none draws nothing. The window keeps what it
shows; nothing about a presented image is withdrawn by not presenting the
next.

**The loop waits for a signal, or one period.** Every change a frame shows
signals `App::cond`: a size, a scale, a state flip and quit did already, and
a queued event (`app_event()`), `fnl_invalidate()`, the output, the hovered
button and the reticle now do too. The period (`pace_period()`, or 1/60 s
where none is known) is the fallback for a change that signals nothing, or
one signalled between the decision and the wait: either waits as long as it
did for the next present. A loop at rest is one lock and a compare a period,
and no frame drawn or presented.

**The frame that answers is drawn at once.** No spacing and no late-latch
hold: the swapchain's queue is empty, so it is shown at the next refresh
however late in the period it is submitted, and holding it would only risk
the one after. What would read the gap as a measurement is restarted there --
the frame-rate window, the spacing's anchor, the slack controller's settle --
so a rest is neither a slow frame nor a long period.

**Three things keep a window drawing.** `REST_AFTER_PRESENTS`, 120: a window
does not rest until its period is learnt, or the first drag after a rest is
spaced by nothing where nothing announces the period (no refresh reported; see
the NVIDIA figures above). `REST_SETTLE`, `MAX_FRAMES_IN_FLIGHT` frames after
each change: a retired swapchain is freed only by submitted frames counting it
down. And a queued event, as before, where there is a callback to drain it: a
caller that never drains records every frame, as it always did.

What it is worth, `pace.py` before and after, the same programs on the same
machines: frames presented and per cent of one core. A size request is a
change, so a rate under the refresh is now drawn at about three frames a
request -- the one that shows it and `REST_SETTLE` after -- and not at 60.

| sizes/s | x11-vulkan | wayland-vulkan | win64-vulkan (wine) | bare@thinkpad, `noshm` |
|---|---|---|---|---|
| idle | 60, 2.3% → 0, 0.3% | 59, 2.5% → 0, 0.2% | 60, 3.0% → 0, 0.5% | 60, 54% → 0, 0.2% |
| 5 | 60, 4.3% → 20, 2.7% | 60, 3.0% → 20, 1.3% | 60, 5.7% → 20, 3.8% | 60, 57% → 30, 21% |
| 15 | 60, 7.2% → 20, 4.8% | 60, 4.0% → 30, 2.5% | 60, 11.5% → 20, 6.5% | 60, 79% → 30, 66% |
| 30 | 60, 9.7% → 60, 8.7% | 60, 5.8% → 60, 5.5% | 59, 14.8% → 60, 13.3% | 60, 98% → 60, 98% |
| 60 | 60, 13.2% → 60, 13.0% | 60, 8.0% → 60, 8.2% | 60, 20.2% → 60, 19.3% | 60, 113% → 60, 113% |
| 250 | 60, 14.2% → 60, 14.2% | 60, 9.5% → 60, 9.2% | 59, 20.3% → 60, 19.3% | 60, 114% → 60, 114% |

The thinkpad's X server took 31% of a core more at rest before, and nothing
after; `fenestal-pace` there had been 50% lavapipe's four rasteriser threads
re-drawing one image, of which a single thread does the work in 24%. The
schenker laptop at rest, 4.2% → 0.5%. From 30 sizes a second up every
frame shows a new size and nothing changes; the border rows are as they were.

**A rest silences `--frame-times`.** Its half-second line restarts with the
frame-rate window at every wake, so a window that draws a size and rests before
the next one prints no line while it is driven -- at 5 and 15 sizes a second
everywhere, and at every rate on Windows once its frames were held until shown.
`pace.py` averaged the lines it had, the settle's, and read 20 or 30 for any of
them, or 10 where the settle left one line (`x11-gl` on this desk). The 5 and 15
rows above were read that way; counted (`fnl_frame_stats()`, which
`fenestal-dev pace` prints for its drive and `pace.py` reads), they are 19 to 20
and 45 to 47 on every machine of 2026-10-30, three to four frames a size, as the
rule says. Cost sampling keeps the window going (`fnl_set_cost_sampling()`), so
the testbed under `--footprint-log`, which turns it on, prints its lines through
a drag.

It is also what a key costs. `stress.py` times an unbound key from the
portal to the echo, on a still window and then with the scene moving.
Before, every window drew, and X11 read 32 ms where Wayland read 16: the
key waited out the X11 turn's spacing, hold and acquire before anything
recorded. Now, at rest and then moving, x11 12 and 12 ms, Wayland 11 and
16, the thinkpad 18 and 30. About 11 ms is the portal and the compositor,
and is in every figure. The thinkpad's moving one is lavapipe's frame:
`--frame-times` there, the testbed's scene moving at 1206x648, reads 26-27 ms a
frame and 38 fps, 28-30 ms and 35 fps under `noshm`, all of it in `present`
and none in pace or hold. A key that lands during one waits for the next
record, half a frame on average.

`fnl_fps()` keeps the rate the window last drew at, since a reading of zero
would be a change and draw a frame to show it. `--frame-times` prints `frame
at rest: 0 fps, nothing drawn` each half second instead of its usual line,
which is what `pace.py`'s idle row reads.

**What a frame costs is the figure for rendering, not the rate.** A window
that draws only when something changed draws at whatever rate its input
and its animation ask for, so `fnl_fps()` says how often, never how fast:
a still window and an idle GPU read the same as a struggling one. So the
library times each drawn frame instead, in two halves, as running means
(0.9 to the old, 0.1 to the new) over the frames drawn, kept while none is
and started again on another device. Each half spans the same thing on
every renderer, since the GPU ranking puts them in one list (*What a frame
costs, compared*, which has the measurement):

- **The GPU's**, from the frame's first command to the end of its
  drawing, the copy path's readback in it and a snapshot's copy not, by
  timestamps each renderer writes into the frame and reads back once the
  frame's own fence has signalled, never waited for (`app_gpu_time()`):
  Vulkan a query pool of two per swapchain image, at the top of the
  image's command buffer and after the copy path's readback -- the buffer
  is submitted again unrecorded while the draw state stands, so the reset
  is in it; OpenGL two `GL_TIMESTAMP` queries per slot, the copy path's
  readback into a pack buffer ahead of the second, and a frame that takes
  a snapshot not sampled; Direct3D 12 a timestamp heap resolved into a
  readback buffer at the list's end, its second stamp ahead of a
  snapshot's copy; Metal the command buffer's `GPUStartTime` and
  `GPUEndTime`, read at the slot's wait and not in a completion handler,
  which could run after the App is gone, and not taken from a buffer that
  carries a snapshot's blit. A device that keeps no time
  (`timestampValidBits` 0) says -1.
- **The render thread's** CPU time, from the end of its waits for the
  device and the display to the return of the call that hands the frame
  over (`app_frame_mark()`): the submit, the swap and KMS's copy path's
  readback and copy in it, a wait that blocks out. The late-latch cap's
  `late_cost` is nearly the same span in wall time, up to the present
  call, which the pacing needs; the figure falls back to a wall-clock mean
  (`App::wall_cost`) where the thread's CPU time is not known (Windows
  under wine).

`fnl_frame_gpu_ms()` and `fnl_frame_cpu_ms()` read them from any thread;
the testbed shows both under its counter, and `1000 / max(gpu, cpu)` as the
rate the slower half allows. Seen on this desktop: the testbed at rest on the
RX 590, GPU 0.07 ms, CPU 0.12 ms, max 8620 fps, under a counter that read 60.

**Published, and in the draw state.** A caller can only draw a figure from
`on_overlay`, which runs only once a frame is already being re-recorded, so
a figure the draw state does not carry is drawn whenever something else
happens to change: late, and after a move to another GPU not at all -- the
first GPU time arrives frames after the move, by which time the window was
at rest with a dash. So the two means are published (`app_cost_publish()`)
into the pair `fnl_frame_*_ms()` read and the draw state latches, and a
change re-records as `fps`'s does. Not every frame's: a figure that moved
with every frame would make every frame ask for the next, and the window
would never rest. While frames are drawn, once a `COST_WINDOW` (0.2 s, the
counter's window); at once where a figure was unknown; and once more after
the window comes to rest, for the last frames' times. Those are read when
the frame is done, which a window at rest never reached by drawing again, so
every renderer answers a non-blocking poll for finished frames
(`gpu_time_poll`), asked every turn of the loop, at rest too. The frame drawn
at rest for the figures alone owes none of its own (`App::cost_counts`), or a
still window would draw five times a second for good. Measured on the RX
590 under GNOME, Wayland, Vulkan and OpenGL, at 0.5 s: the line moved twice
a second while the scene moved, and the testbed at rest, after a move to
lavapipe and back with no input after either, used 0.0 to 0.3 % of a core,
with both figures shown.

**Measured at rest only where the caller asks.** A window at rest draws
nothing, so the figures stay the last drawn frame's however long it rests.
A caller that shows them can keep them live: `fnl_set_cost_sampling()` has a
window at rest draw one frame every `COST_WINDOW` -- the recording it has,
or the overlay again where the published figures moved, which in practice
is every time, so it is one re-recorded frame five times a second -- and
every such frame counts (`App::cost_counts`). The frame-rate counter goes
on counting through the rest then, and reads what is drawn, five frames a
second; its window and the publishing are one (`fps_update()` says when a
window closed, and `app_cost_publish()` publishes with it), so the three
figures move in one re-record rather than two out of phase, and a change to
the counter alone is a change to the figures (`draw_state_update()` leaves
it out of what counts as moved). Neither that frame nor one drawn for
the figures alone restarts `REST_SETTLE`: no swapchain was retired, and three
frames a time would triple what sampling costs. Off by default, since a
window nobody is reading the figures of should not pay for them, and the
metrics panel shown does not turn it on (*The metrics panel*); the
testbed turns it on for `--footprint-log`, and for a benchmark run unless
its `cost-sampling` option is off (on by default). Measured on this
desktop, the testbed at rest with the panel up when the panel sampled:
at 0.5 s, 0.2 % of a core on the RX 590
and 13 % on lavapipe, whose frame is rasterised on the CPU; at 0.2 s, where
`fnl_fps()` reads 5 at rest and a steady 60 with the scene moving, 0.6 %
on Wayland Vulkan, 0.8 % on Wayland OpenGL, 0.4 % on X11 Vulkan and 30 %
on lavapipe; with the panel down, 0.2 to 0.4 % throughout. `--frame-times`
keeps a half-second line on a clock of its own (`FRAME_TIMES_WINDOW`),
each with the frames its window drew: `pace.py`'s border row adds up the
lines that arrive under the press and divides by the time they span
(`frames_between()`), so a rest inside a drag counts as time with no
frames and the lines after the release count for nothing.

### Late latching

The frame's state is sampled after most of the display period it will sit
through has already passed, so what finally appears is that much newer. On by
default; `--no-late-latch` disables it and `--late-latch=<ms>` pins the delay.

The hold is portable. How long to hold for is arrived at two different ways,
because the window servers do not offer the same signal, and
`FnlBackendVtbl::invites` picks between them.

**Where frames are invited (Wayland), the delay is probed for.** Creep it up,
and treat an invitation arriving two periods after the last as having crossed
the line. The invitations are quantised to the compositor's own frame, so a
commit that missed one is answered a whole period late — which is what a present
timestamp would say, without a wait the loop cannot be interrupted out of. Two
things took measuring to get right: the period estimate has to survive both
kinds of outlier, so only gaps that look like a period are averaged in; and a
single miss is not evidence, since compositor hiccups produce them at any delay.
The cap is `period - 2 ms - 1.5 × cost`, and it rather than the misses is
normally what stops the walk. Probing does not start until the window has been
presenting for a second, and only ordinary frames are delayed — a rebuild is
answering the compositor and goes out at once.

**The quantising is the compositor's, though, and not every one keeps it.**
weston holding a late commit for a FIFO barrier or a commit time -- as it
does for NVIDIA's WSI, which uses both -- answers it a little late instead:
on schenker's GTX 1050 Ti (580.178.04, 2026-10-19), with `WAYLAND_DEBUG`
beside `wp_presentation`'s own times, the screen's gaps stayed 16.7 and
33.3 ms while the invitations' slid, 17.3, 18, 20, and 24 to 27 for a
missed cycle, about the 1.5 periods a miss is read at. Averaged in, the
slide raised the period and the cap with the hold, which walked past the
period itself (17.7 ms) and drew `pace.py`'s draw row at 40 fps; a pinned
8 ms or no hold drew 60. So the gaps are also read as the measured
controller reads its turns: averaged (`App::late_turn`, the same weight)
against the period, longer by `LATE_SLIDE` and the hold is cut under its
ceiling. Only gaps inside that margin teach the period, and a longer one is
adopted only when nothing over `LATE_HOLD_MIN` is held, so a slower display
is still learnt. The margin is 3%, not `LATE_OVERRUN`'s tenth: a gap is
the frame rate, where a turn is not, and at a tenth the same row sawed
between 48 and 60. At 3% it held 59.6 and 59.8 fps in two 30 s runs, the hold at
10 to 12 ms; GNOME's draw row stayed at 60, its hold cut once from 14.3 ms
after a 58 fps half-second and creeping back.

**Where they are not (every backend but Wayland), the block is measured.** With
no invitation to wait on, the loop runs straight into `vkAcquireNextImageKHR`,
which does not return until the presentation engine releases an image — and how
long it sits there *is* the spare time in the display period. The delay is
`(held + blocked) × LATE_SLACK_SPEND`, which is 0.75; the remaining quarter is
headroom for a frame that costs more than the one measured. `late_latch_slack()`
in `src/core/frame.c` is the whole of it.

At the fixed point the delay is three times the block, since `d = 0.75(d + b)`
solves at `d = 3b`. Hold too long and the queue drains, the block reads zero and
the delay is cut by a quarter; and on a GPU-bound frame nothing is ever queued,
the acquire never blocks, and the same factor decays the delay to nothing —
correctly, because there the CPU's idle is the fence wait, which is the GPU
being behind rather than the display being ahead.

**That argument holds only while holding longer makes the block shorter, and
neither figure says whether it does.** Two things break it, and both read as
*more* slack the more is held. A hold that comes back late: the sum counts the
overshoot, so a wait that asked for 11 ms and took 25 argues for a longer one.
And a present that blocks on work rather than on the display, which no hold
shortens at all. Measured in the Windows guest, where both are true at once —
lavapipe's present is the rasteriser, and the wait was on a 15.6 ms tick — the
testbed ran at 30 fps with `--no-late-latch` and at 20 without it:

```
frame 33.52 ms: pace  0.01 ... present 33.35 (hold  0.00, 30 fps)   --no-late-latch
frame 57.70 ms: pace 41.56 ... present 17.54 (hold 25.23, 17 fps)
```

So held turns are read against what a turn costs with nothing held
(`App::late_base`, the same three figures summed over the turns whose hold was
zero, of which the first `LATE_BASE_TURNS` are taken before anything is held).
Read as an average (`App::late_turn`) and never one at a time: on X11 a long
turn and a short one arrive as a pair with nothing wrong, 19.6 ms then 13.8,
and a rule that counted single turns past a tenth walked an 11 ms hold down to
1 ms inside five seconds. An average more than `LATE_OVERRUN` times the base
is holds lengthening turns, and the delay is cut under a ceiling that creeps
back, which is the probe's answer to a missed cycle. A delay under
`LATE_HOLD_MIN` is not waited for, so one cut far enough is no hold, and the
turns that follow are unheld ones that keep the base current. The same margin
the other way is a faster display, and the base follows the average down. The
margin is a tenth and not a half, because a missed cycle does not show as two
periods here: the drained queue lets the acquire through, and the turn is the
hold alone.

A software device is left out before any of that. Its present waits for the
frame to be drawn, so `App::present_block` stays unmeasured on a
`VK_PHYSICAL_DEVICE_TYPE_CPU` device and late latching is inert there, as it is
for any renderer that cannot measure the block. Inert means no hold only
because the figures start again with the device: a move forgets the hold,
its ceiling and the base (`late_slack_forget()`, as a wake from sleep
does), where nothing invites. Kept, the hold a GPU had learnt stood for
good on the software device after it, where nothing measured could cut it,
and cost far more than its length. On schenker's KMS at 3840x2160@30,
llvmpipe after NVIDIA's OpenGL held 13.8 ms a frame and drew 10 fps; with
no hold it drew 30. The work was the same: a short wait each frame with
every core idle kept the CPU's own clock choice (HWP) at 1.1 GHz with
four cores' worth busy, at 5 W and with no limit reported, where llvmpipe
started there ran at 3.4 GHz, and holding the clock at 3.4 GHz gave the
30 fps back. The GPU ranking measured the same slow state, 11 and 10
fps for OpenGL's and Vulkan's software entries, since a calibration moves
to each; with the hold forgotten, two calibrations there measured 31 and
50 for OpenGL's and 38 and 32 for Vulkan's. That spread is the machine's:
under its energy preference `balance_performance` HWP drops the clock to
1.1 to 1.6 GHz for seconds at a time under llvmpipe's load, still with no
limit reported (llvmpipe at 15 fps there), and under `performance` holds
3.4 GHz (30 fps throughout). A figure for a CPU rasteriser is as steady
as the machine's clock policy.

`App::present_block` therefore excludes every fence wait and includes the
present call as well as the acquire, because which of the two a driver blocks in
is the driver's business. It is the renderer's measurement rather than something
the loop could time from outside `draw_frame()`, and that is the whole reason it
is a field on the contract. A renderer that cannot measure it leaves the
sentinel negative, and late latching degrades to inert there.

The measured controller ignores `LATE_SLACK_SETTLE` frames after a swapchain
rebuild. A new swapchain has no presentation history, so it is not only the
first present that goes out unthrottled — the queue stays refillable for as many
frames as it has images, and every one reads a block of nearly zero. Eight
frames is above any image count a swapchain here is built with.

`late_cost` — the frame's own cost, which the cap subtracts 1.5× of — must be
measured from the far side of the device waits, not from `App::latch_time`.
Where the throttle sits *between* the latch and the present, measuring from the
latch charges the whole display period to the frame, the cap goes negative,
clamps to zero, and the delay never leaves the floor.

Wayland does not use the measured signal because it has nothing to measure: the
frame callback is that backend's throttle and the acquire is not.
`--frame-times` said so outright, in its format then: `pace 16.4 wait 0.09`
on the Wayland build against `pace 11.5 wait 5.1` on X11.

Measured on a 59.95 Hz display, the testbed's scene: the hold removes about
11.5 ms of staleness from a 16.7 ms period, and the period does not move — frame
time is the same to within 0.06 ms with the hold on and off, and the sub-60
second count is the desktop's own noise floor either way. Both controllers
settle in the same band, one by crossing the edge and one never touching it.

Three alternatives were turned down and are worth not re-proposing. The
acquire's timestamp fed to the *probe*: a FIFO acquire is quantised only while
the queue is full, so hold long enough to drain it and the gap slides smoothly
instead of doubling — the signal goes soft exactly where it has to be sharp.
`VK_GOOGLE_display_timing`: a real per-present timestamp, implemented by Mesa on
X11 and broadly not by Windows drivers, so it solves half the problem in
exchange for a second controller to fall back to. `VK_KHR_present_wait`: a
stronger fact, answered by *blocking*, which is the one thing the invitation was
preferred for not doing.

**No period yet is not "unthrottled", except where it never will be.**
Where nothing invites, the loop spaces turns by the output's refresh when
the measured period is under `PACE_UNTHROTTLED` of it, since that is a
swapchain throttling nothing (`pace_period()`). With no period yet it did
the same, and on amdgpu's display path (this desktop's console, RADV,
60 Hz) that starved the measurement: spaced every turn, FIFO never fills,
and the few turns the spacing did not hold block for 3 to 9 ms, which is
what the period samples sum -- the early learner settled on 7.5 ms, the
estimate fell to about 2 ms and stayed, since a sample over 1.5 times the
estimate is dropped as a missed cycle, and the hold went to 0, late
latching off. `FNL_COST_PARTS`'s `period samples` line showed it. The
rule "no period, space every turn" is load-bearing in one place: a
software device's present waits for its rasteriser and is never sampled,
so there "no period yet" is for good, and the spacing is what holds
lavapipe on a bare X server to 60 rather than 135 (measured, the one time
the rule was dropped for all). So each renderer says which it is
(`App::present_untimed`, from what it already knew: Vulkan's
`phys_software`, OpenGL's `software`, Direct3D 12's WARP), the spacing
keeps the old rule there, and elsewhere leaves the swapchain to throttle
until a period is learnt. And the threshold is 0.6 of the refresh, not
0.9: the probe's deliberate miss puts a few short samples beside it,
which took a correct 16.5 ms estimate to 13.7 (0.82), under 0.9 and into
the same starvation; a swapchain that throttles nothing reads far under
(lavapipe's 7 ms, 0.42).

**An estimate that says nothing blocks is put right when the samples say
otherwise.** Mesa's OpenGL on schenker's Xorg (modesetting with glamor,
the HD 630) lets a new window's swaps through unthrottled for about half a
second -- 254 frames a second -- and blocks for the refresh ever after. The
sixty presents the estimators wait out end inside that burst, so the first
settled sample was the frame's own cost, 1.06 ms, and it replaced the
estimate whole: under `PACE_UNTHROTTLED`, so the loop spaced every turn by
the refresh, which samples nothing, and the hold stayed at 0. With the
spacing switched off for a probe, the swap blocked 16.1 ms a frame and the
estimate still read 1.06, since every true sample is over `LATE_MISS_FACTOR`
of it and was dropped as a missed cycle. So two things, both only while
the estimate is under `PACE_UNTHROTTLED` of a reported refresh: eight
samples in a row that it would drop, agreeing with each other, replace it
(`PERIOD_RELEARN_RUN`; a missed cycle is one sample, never eight alike, and
an estimate at the refresh is never touched, so a device falling behind
for a while is not taken for a new display); and while the spacing holds
every turn on a device whose present is timed, a burst of eleven turns is
let through unspaced after 120 spaced ones, the gap doubling after each
burst that changed nothing to 3840 turns (`period_probe()`), so the swap can
say it blocks. On schenker the period is learnt again 2.3 s into the window
("display period learnt again: 16.62 ms, where 1.07 stood") and the swap
throttles from then on; the stress round's window had stuck at 3.25 ms and
was put right the same way. The hold there settles at 0 to 1.1 ms and not
at Vulkan's 12: a longer one makes that swap miss its vblank, and the
controller cuts it, which is the controller working. What it costs: the
callbacks' intervals move with the hold, p99 17.67 ms against 16.83 with
the spacing; whether what reaches the screen moved, X11 has no present
stamps to say. Vulkan on the same server, KMS and weston never reach the
rule.

**And where the swap never blocks, the probe's bursts are the cost.**
Mesa's OpenGL with `vblank_mode=0` on the same machine is a hardware swap
that throttles nothing with vsync asked for: the estimate stays at the
frame's own 1.17 ms, which is right, the loop spaces every turn by the
reported refresh, and over a 20 s measure it drew 1221 frames where the
rate is 1200 -- 61.05 fps, the extra 21 the probe's bursts of eleven
(24 frames with under 1 ms of pace), each burst's gap having doubled
toward 3840 turns. Nothing else of it shows: the callbacks' intervals
p50 16.66 ms and p99 16.82. With the swap blocking as it should, the
same window learnt its period again (16.84 ms, where 1.00 stood) and
drew 59.9995 fps.

**Nor is a missed cycle taken for the period.** The first sample after
rest can span two refreshes, and the first settled one replaces the
estimate whole: one round of `pace.py`'s border row in four on this desk's
`x11-vulkan` (at `ee3e917`) learnt 33.20 ms there and spaced the whole
drag to it, 32 fps, since a rebuilt swapchain blocks on nothing and no
sample came to put it right. Where the output reports its refresh, a
sample over `LATE_MISS_FACTOR` of it is a missed cycle, and neither the
early learner nor the first settled sample takes it (`period_missed()`);
the estimate already learnt drops such samples as before. Not seen at
`d3e64bb` in six rounds, the rule stands on the path, not on a figure.

Measured after, 2026-10-28: amdgpu's display path, the estimate 14.9 to
16.9 ms over 45 s and the hold 4 to 11 ms, where it had been 2 ms and 0,
OpenGL there 16.63 ms and 5 to 12; schenker's KMS, all four of Intel's
and NVIDIA's Vulkan and OpenGL at 16.6 ms with holds of 6 to 12 --
Intel's Vulkan on its own panel had learnt no period at all before, hold
0; GNOME's X11 Vulkan at 16.6; aspire's Windows, D3D12 and Vulkan on both
GPUs, the same as before the change; lavapipe on thinkpad's Xorg still
held to 60. The console's own dip every 21 s (below the library, TODO)
now costs frames of 17.8 to 18.5 ms rather than 21 to 25: the controller
backs off from a period it knows.

On the Mac, where Cocoa invites nothing and Metal measures every present,
the change was expected to alter nothing, and in frames it does not: two
`pace.py` sweeps on schenker's HD 630, its panel alone (2026-10-28 at
`dd53eff`, `docs/benchmarks.md`), draw every row at the rate the sweep of
2026-10-22 did, Metal and Vulkan alike -- 20 at 5 and 15 sizes a second
as `pace.py` then read them (45 to 47 at 15, counted: *At rest*), 60
from 30, 60 drawing and at the border. Their CPU agrees between them
and not with that sweep: Metal's at 30, 60 and 250 sizes a second is 20
to 36% and 69%, against 18, 25 and 60, while Vulkan's is within a few
points. That sweep is single and about 200 commits older, so the rise is
not placed on this change. Three sweeps back to back at one commit
(2026-10-31, `1d987ac`) say how far one moves: Metal's agree within 1.6
points of a core from 5 to 60 sizes a second and 7 at 250 (Vulkan's
within 1.4 and 0.9), at 19 to 30% from 30 to 60 and 58 to 65% at 250,
below Vulkan's at every rate, the frames the same in all three. So the
spread between the earlier sweeps is between sittings, not one sweep's
noise; OpenGL's column is the noisy one, two cells of twenty-one a half
over the rest.

A third sitting at `a24e42f` lay between the two, 19 to 31% at 30 to 60
sizes a second and 56% at 250, below Vulkan's again, and a fourth sweep
of the same library a day later read 2026-10-22's, 18 to 24% and 51%.
Three on another day (2026-11-06, `e31d865`, macOS 26.7.1, both panels
lit where the earlier sweeps had one) say the sitting itself does not
drift: 5 and 15 agree within 0.5 points of a core, 30 and 45 within 1.9,
and 250 within 2.6, while the 60 cell moved 7.3 (23.7, 29.3, 31.0) as
October's 250 cell moved 7 -- the noise of a sweep is one cell's, and
which cell changes; the level does move between sittings, 250's 52.5 to
55.1 below October's 58 to 65.

### Vsync and present modes

A window is vsynced, on `FIFO`, for its whole life, and neither a caller
nor a key changes it. Off is a diagnostic: `FNL_VSYNC=0` in the
environment, read once at `fnl_init()` as `FNL_ECHO_EVENTS` is, or
`--no-vsync` to the testbed and to `fenestal-dev pace`, which set it. Off asks
for the fastest mode the surface offers — `MAILBOX`, which does not tear,
or `IMMEDIATE` — and keeps it. It is kept for what only a window drawn
flat out shows: a present path that waits where the GPU does not, which
is every figure here taken under `fenestal-pace --draw --no-vsync`. What
a frame costs is `fnl_frame_gpu_ms()` and `fnl_frame_cpu_ms()`, vsync or
not, and the stress round measures the off-screen skip on a vsynced
window drawing every refresh (`DRAWING_FPS` in `stress.py`).

The mode is chosen at each swapchain build, a rebuild after a loss
included, from what the surface offers then, so the status line always
reports the mode the swapchain was actually built with.

A tearing mode presents as fast as it can and so would never notice the window
leaving the screen. It falls back to the same wait once nothing has invited it
for `FRAME_WAIT_S`.

`FnlRendererVtbl::present_mode_matches()` asks "does the swapchain that exists
already present the way `vsync` asks", and a false is what makes the loop
rebuild. It is a call and not a comparison because what a present mode *is*
differs between graphics APIs — FIFO against MAILBOX here, a sync interval plus
a tearing flag elsewhere, a boolean on Metal. The loop only wants to know
whether it must act.

### Recording

Command buffers are generation-gated: one is re-recorded only when the
`FnlDrawState` it was built from actually changes, and a static screen is not
drawn at all (*At rest*). `FnlDrawState` holds three kinds of thing and
nothing else:

* what the library draws — the titlebar's inputs: focus, maximize, fullscreen,
  the hovered button, the title;
* what the library *knows* and draws — the outputs the surface is on and their
  size (which `FNL_EVENT_OUTPUT` also reports), the reticle, the figures the
  metrics panel shows;
* what the frame is drawn from — the scene's size and antialiasing, its
  animation time, the veil.

Everything a caller *can* observe is covered by `user_gen`, which
`fnl_invalidate()` bumps. A field added here for the sake of a caller's widget
is a library keeping state about a program it cannot see.

**A queued event is a third reason to record**, and it has to be. An
application drains in `on_overlay`, `on_overlay` runs only from a re-record,
and most events move no field above: a key the library does not bind, a scroll,
a button release, a press on a rectangle a custom titlebar claimed, a pointer
crossing anything but the library's own buttons. Without this the state
compares equal, nothing records, and the event waits for something unrelated to
move the frame — on a still window, for the FPS reading to flicker, which is
up to half a second and differs run to run. `fnl_invalidate()` cannot close it
either: the only place a caller could call it from is the drain that is not
running. It is the closed chain `FnlDrawState::fps` documents for one field,
generalised, and it takes the same answer — give the loop a reason to record
that the caller had no way to supply. Gated on there being a callback to drain
it — `on_overlay` or `on_record`, either counts — so a caller with neither does
not record for ever on the first event; and it costs a still window nothing,
because a still window has an empty queue.

**Whether a turn draws is the core's; whether drawing calls `on_record` is
the renderer's.** Everything above decides to draw or to rest, the same for
every graphics API. A renderer that keeps a recording -- Vulkan's command
buffers -- re-records an image only when `record_gen` has moved past the
one it was recorded at, and submits it again otherwise. One that does not
records on every turn the core draws, and never on one it rests: OpenGL,
where a frame is the calls that drew it, and Direct3D 12, which could keep
a list per back buffer and does not. One list, reset each frame on the
slot's allocator, is GL's shape, which the D3D12 renderer shares the
scene's and the overlay's drawing with; a list kept per image would want
Vulkan's retirement of what each names across a resize, for a saving that
recording a few dozen draws does not need. The public
promise is what both keep: `on_record` and `on_overlay` run at least when
something they draw from has moved, and how often beyond that is the
renderer's, said in its interop header. No seam entry carries the
difference, because nothing in the core would read it; a renderer that cannot
keep a recording still draws nothing on a turn the core rests.

`fnl_draw_state_eq()` compares field by field, not with `memcmp` — the struct
has padding that is never written, so a byte compare would re-record every
frame. A `_Static_assert` on `sizeof(FnlDrawState)` is the tripwire: adding a
field fails the build and forces a look at the comparison.

The overlay is three primitives — solid rectangles, antialiased strokes and
glyphs — and each is **one draw call for the whole frame**. Everything is
appended to per-image instance buffers while the layout runs, and drawn by kind
at the end.

Each stream has a ceiling, and the ratio between them is the figure that
matters: 4096 glyphs, 1024 rectangles, 512 strokes, which is 248 KB an image and
1.9 MB across the eight. The library's own bar spends about 800 glyphs with
everything on, some thirty rectangles and twenty strokes, so what the ceilings
really size is the caller — and because every filled rectangle in this API is a
background, a caller building an interface spends them a panel, a row and a cell
at a time and reaches the rectangle ceiling first. An append past one draws
nothing and reports nothing, so headroom belongs where it will actually be
needed; a rectangle count a quarter of the glyph count had it in the other
place.

**What makes that legal is an invariant, and once the overlay is public it is
the caller's too: every filled rectangle is a *background*.** Nothing solid is
painted over a stroke or a glyph, so collecting the three streams and then
drawing rects, strokes, text puts each primitive exactly where drawing them in
call order would have. Within a stream, instance order is primitive order, which
Vulkan guarantees. `fnl_overlay_flush()` is the escape hatch, and it is why this
is three draws a frame rather than free.

The library itself flushes once, at the end of `draw_overlay()`. A caller that
needs its own layering inside that calls `fnl_overlay_flush()`, and a caller
that needs a scissor — the testbed's crosshair is clipped to the presentable
area — calls `fnl_overlay_clip()`. Either way a stream simply flushes twice, the
second picking up where the first stopped via `firstInstance`; nothing is copied
or rebound.

`fnl_overlay_clip()` flushes the rectangle and stroke streams itself, before it
changes the scissor, and it has to: a scissor is command state and applies to
the *draw*, not to the append, so a rectangle appended before the call would
otherwise be issued after it and come out clipped by a clip asked for
afterwards.

**Text is not clipped by it**, and that is the invariant's cost rather than an
omission. Glyphs are one draw for the whole frame, so the only scissor they can
have is the one in force when that draw is issued, and the frame takes back a
clip left in force before it issues the glyphs, so a caller's clip never
reaches text. A layout that has to keep text inside a rectangle knows which
lines fall outside it and should not draw them. Splitting the glyph stream on
each clip change would fix it and would cost the property the stream exists for.

The rectangles do not blend. They stand in for a clear, and a clear writes;
blending them would composite each against the load-op clear rather than
replacing it. The one thing still drawn with `vkCmdClearAttachments` is the
transparent grab ring, which wants exactly that — a write of alpha zero, four
rectangles in one command, before anything else in the pass.

A re-record costs about 136 µs with the whole overlay up (RADV, Polaris), two
thirds of it the driver's command-buffer management rather than anything the
overlay does. At 60 Hz that is 0.8% of a frame, so the remaining lever is to
record less often rather than to record faster. Only the counts are baked into
the command stream — the instance *data* lives in host-visible buffers — so a
frame that only moved the crosshair could rewrite the data and reuse the
recording. That is deliberately not done: it trades a legible "the command
stream is unchanged" invariant for half a percent of a frame.

Two smaller things on the same path. The palette is authored in sRGB and
converted once when the surface format is resolved, rather than three `powf()`
per primitive per frame. A *caller's* colours are not a table and cannot be
converted once, so `to_surface()` memoises the last four instead. Drawing
arrives in runs of a single colour, a panel and every row in it, but as often
in a pair that alternates — a label and its value down a readout — and one
entry missed every call of the second: `powf` was 1.4% of an unthrottled
frame with the testbed's panel up. Keyed on the blend space rather than on the
window, since that flag and the colour are the whole of what the answer depends
on and a device-loss recovery can re-resolve the surface format under the same
`App`. Thread-local, because it runs ahead of the check that makes appending
single-threaded. And the quads are
four-vertex triangle strips whose corners are the low two bits of
`gl_VertexIndex`, not six-vertex lists indexing a local array.

### Per-image instance buffers, and the fence a rebuilt swapchain forgets

Per-image instance buffers are **device-scoped, not swapchain-scoped**, because
a rebuild retires command buffers that still name them. There are
`OVERLAY_MAX_IMAGES` of them for that reason — the image count can change under
a rebuild, so they are sized for the ceiling rather than for the swapchain in
front of them — but they are *one* `vkDeviceMemory` and *one* mapping apiece,
strided, which is what `create_instance_buffers()` in
`src/renderer/vulkan/pipeline.c` does. Eight separate allocations and eight
separate permanent mappings per stream say nothing the one pair does not.

Everything that exists once per swapchain image is sized by the same ceiling:
the views, framebuffers, render-finished semaphores, command buffers and
`images_in_flight` are fixed `[OVERLAY_MAX_IMAGES]` members rather than
allocations, so a rebuild has no allocation of its own to fail and a retired
swapchain carries its arrays by value. A surface that wants more images than
the ceiling is refused, and `image_count` is set only after that check: two
images on one instance buffer is tearing that shows only under load.

`images_in_flight` describes frames, not swapchain objects, so a rebuild
resets it — and resetting it to null is the mistake. The re-record guard skips
a null fence —
correctly on a first swapchain, where nothing has been submitted, and wrongly on
a rebuilt one, where it is skipping a wait that is owed. `draw_frame()` waits on
`in_flight[frame]`, which covers one of the two frame slots; the other slot's
submission can still be in flight. So for exactly one frame per image after a
resize, recording could write instances a retired command buffer was still
reading.

The array is therefore seeded with the *other* frame slot's fence rather than
with null, in `image_frames_reset()` in `src/renderer/vulkan/swapchain.c`,
which a swapchain made and a resize in place both call:

```c
VkFence seed = VK(a)->in_flight[(a->frame + MAX_FRAMES_IN_FLIGHT - 1) %
                                MAX_FRAMES_IN_FLIGHT];
```

Conservative, and it costs no stall: it is a frame that had to finish before
this one anyway. Null on the very first swapchain, since `create_sync()` has not
run yet. A `vkQueueWaitIdle` in `recreate_swapchain()` would also work and is
exactly what the retired-swapchain machinery exists to avoid.

The cost of getting this wrong is invisible — one torn frame of glyph instances
during a drag-resize — and sync validation cannot see it at all, since it does
not track host writes to persistently mapped memory. Content putting anything
less forgiving on the same per-image pattern would make it visible.

Retired swapchains are freed once `MAX_FRAMES_IN_FLIGHT` further frames have
been submitted; a burst deeper than `MAX_RETIRED` forces an early flush.

Each is freed as it expires, and **batching the drains was tried and turned
down**. The destroy needs a `vkQueueWaitIdle` — Vulkan 1.0 has no fence for a
present, so queue idle is the only evidence a retired `render_done[]` wait has
retired — and a drag-resize retires one swapchain per configure, so that is one
full CPU/GPU rendezvous per frame for the length of a drag. Holding expired
entries until the whole set goes quiet folds them into one drain per
`MAX_RETIRED`, which is the obvious-looking win.

It buys nothing once the rebuild spacing above is in place, because that already
bounds a drag to one rebuild per display period, and at that rate a drain costs
what an already-idle GPU costs: 157 ticks against 154 over ten seconds of
dragging, at both 60 and 120 configures a second, which is the noise floor. The
cost is not noise — `MAX_RETIRED` swapchains alive through every drag instead of
two, an image count of colour and depth buffers each, on the path whose answer
to a failed allocation is to rebuild again. A bound the design budgets for is
not a bound to spend for nothing.

`retired_destroy()` is safe on a half-built entry, and `destroy_swapchain()` is
the same function on the live one: whatever a build that stopped part-way did
not make is null, and destroying null does nothing. The command buffers are the
exception — `vkFreeCommandBuffers` with nothing to free is invalid usage — and
they are allocated all at once, so the first answers for the rest.

### A swapchain larger than the window

**A size costs a swapchain and a depth image, and some drivers allocate
slowly.** On the Raspberry Pi 4 under weston a resize of `fenestal-pace`
spent 21 ms of a 16.7 ms period rebuilding, timed around each step: 14 ms
in `vkCreateSwapchainKHR`, 6 ms making the depth image, 1.1 ms in the
drain before a retire. Every frame missed its refresh, took 33 ms, and so
always found a new size waiting: 30 fps from 30 sizes a second at three
quarters of a core, where the Pi's Xorg, rebuilding every other frame,
held 58. `perf` on the Pi put 88% of the program's time in the kernel: a
third in `memset`, under `dma_alloc_attrs` from the display driver's
buffer allocation, which Mesa makes for each swapchain image a compositor
might scan out; 7% in `clear_page`, the depth image's fresh pages. So the
cost is allocating and clearing memory per size, and weston's repaint
window has no part in it.

**The depth image outlives the swapchain** (`depth_ensure()`): kept while
the extent fits inside it, grown a 256-pixel step at a time when it does
not, since a framebuffer may be smaller than its attachments and the pass
clears depth over its render area alone. One grown out of goes with the
newest retired swapchain, the last entry to expire of those whose
framebuffers name it. 6 ms a size went to 0.01, and alone moved the Pi
from 30 fps to 31.

**On Wayland the swapchain is larger than the window too.** A Wayland
surface leaves the extent to the client, so a swapchain may be any size,
and the surface's size is the viewport's destination: the window shows
the top-left `extent` of a larger image through the viewport's source
rectangle (`crop_apply()` in `wayland.c`), sent just before the present
whose commit carries the image. A size inside the images is then the
framebuffers and their command buffers, retired as a swapchain is, and no
new swapchain (`resize_in_place()`). The images are the window's size
rounded up a 256-pixel step (`app_buffer_room()`), never larger than the
output -- a window the output's size has images its own size, which a
compositor can put on a plane -- and made again when the window outgrows
them or falls under half of them. A swapchain said suboptimal is always
made again: weston changes its format feedback for a buffer it cannot
scan out, Mesa then says suboptimal until a new swapchain, and drawn into
in place the window rebuilt every frame and drew 30 fps at rest. The Pi
4 under weston, `pace.py`, frames presented and CPU:

| sizes/s | before | the depth image kept | and the swapchain |
|---|---|---|---|
| idle | 0, 0.3% | -- | 0, 0.3% |
| 15 | 44, 43% | -- | 48, 15% |
| 30 | 30, 73% | 32, 49% | 59, 21% |
| 60 | 30, 74% | 30, 50% | 60, 26% |
| 250 | 30, 77% | -- | 58, 27% |

The round passes on weston on the Pi, thinkpad and aspire (56, 46 and 65
checks). On schenker's HD 630 under weston, which keeps 60 either way,
it is the CPU that goes: 27.0% of a core from 30 sizes a second and
44.9% from 250 the commit before, 5.6 and 8.3% with it (`pace.py`,
2026-10-31, the two built side by side and run on one boot). It is what
the benchmarks' weston columns fell by between their sets, the Intel,
NVIDIA and NVK GPUs alike, and this desktop's GNOME under Vulkan less.

**Direct3D 12's buffers are made larger the same way**
(`d3d_window_images()`). `ResizeBuffers` is 18 ms a size on Intel's
driver; with the buffers larger than the window a size inside them is
drawn as it is, and `DXGI_SCALING_NONE` shows them from the window's
corner, one to one, and clips the rest. Not through `SetSourceSize`,
which was the first way tried: on a window's swapchain it stretches the
part it names over the whole buffer, and the testbed came out magnified
by 1792/1600 -- the buffer's width over the window's -- and cut off at
the right and the bottom. schenker's HD 630 and GTX 1050 Ti went from 45
and 53 fps from 30 sizes a second to 60 and 60, at 3 to 10% of a core.

**At rest the images are the window's size.** Larger ones are a drag's,
not a window's: Windows flips onto a plane only a buffer the window's
size, and composes the rest, a refresh more on every frame (schenker,
36.8 ms from latch to screen against 20.2; *Benchmarking*). So once a size
has stood BUFFER_SETTLE_S, half a second -- longer than the gaps between a
drag's sizes, a frame or two -- the loop rebuilds once
(`app_buffer_at_rest()`, `App::buffer_settled`), and the room and its test
answer the window's own size; the next size of a drag makes larger ones
again. A window's first images are made at rest, so a window never
resized has its own size from the start. One `ResizeBuffers` at the end
of a drag, 18 ms on Intel's driver, while nothing moves. On Wayland it
changes nothing measured (2026-10-30, the presentation feedback's
zero-copy flag counted over 720 frames, the bench's window held at
rounded images against settled ones): weston scans the larger image out
through the plane's crop as it does one the window's size, zero copy
either way on schenker's HD 630 and GTX 1050 Ti (11.9 and 13.2-13.4 ms
from latch to screen both ways), and GNOME composes a window whatever its
images (none zero copy, 34.7 ms both ways) and scans out a fullscreen one,
which has the output's size anyway (710 of 720, 7.9 ms). It is kept there
for a compositor that does what Windows does, at a swapchain a drag.

**OpenGL on Wayland keeps the same room.** The `wl_egl_window` is
`app_buffer_room()`'s size while the size is changing and the window's
own at rest, and an unchanged size is not handed to
`wl_egl_window_resize()` again: Mesa allocates the next buffer for a
size it is given. The viewport's source is the top-left extent, which
is the part Vulkan's images already show, and the blit lands in the top
of the window framebuffer because that framebuffer's origin is the
bottom. What that saves, where OpenGL draws faster than it reallocates,
is unmeasured. On the Pi the `wayland-gl` window under weston draws on
llvmpipe, and the rasteriser is the ceiling (TODO).

**Where an X11 device can present one to one, Vulkan keeps its
swapchain too.** The surface names the window's extent, so an image of
another size is out of date unless the present mode offers
`VK_KHR_swapchain_maintenance1` (or the EXT name) with one-to-one
scaling and minimum gravity on both axes, and the image lies inside
that mode's scaled extent. The create chains
`VkSwapchainPresentScalingCreateInfoKHR`, and `resize_in_place()` is
the one Wayland uses: `app_buffer_room()` while the size changes, the
window's own size at rest. Queried for the mode about to be used; a
mode without the bits leaves the capability for one that has them. A
device or a surface without them -- Intel's, in *Measured on Windows
itself* -- recreates the swapchain at each size. Win32 is not this
path: its answer is *Vulkan through DXGI* (below). A swapchain made so
that comes back out of date or suboptimal is the driver's refusal,
said once, and sizes recreate it from then on
(`FNL_FAULT=swapchain-scale-refused`). What a drag then costs is
unmeasured.

### Device and surface loss

`VK_ERROR_DEVICE_LOST` and `VK_ERROR_SURFACE_LOST_KHR` are both recoverable
rather than fatal. What survives either is the `VkInstance` and the native
window — and those are what tie the client to its window, so the window stays
mapped and the compositor sees a gap in the frames rather than a client going
away.

The two share one rebuild, differing only in whether the `VkSurfaceKHR` is
replaced first (after the swapchain that was using it, never before). A lost
surface takes the device with it, which is more than it strictly needs: the
swapchain format is resolved *from* the surface, and the render pass and the
pipelines are built on that format. Rebuilding the lot is the only version of
this that cannot be subtly wrong, for an event that happens approximately never.

Recovery probes the surface with a capability query before walking into the
rebuild: neither `pick_device()` nor `query_surface_info()` can report a lost
surface — they would read it as "no device can present" and "no formats" — so
the one call that can report it goes first.

The frame path records the loss and unwinds to the top of the loop, where
nothing is half-recorded and no swapchain is half-retired. Recovery tears down
everything below the instance and builds it again through the *same* pair of
functions startup uses, so the two cannot come to disagree about what the set
contains. The physical device is re-picked rather than reused: a loss can mean
the GPU itself is gone.

Two details. Destroying objects is the one thing that stays legal on a lost
device, which is what makes the teardown safe — the idle waits on the way out
report the loss rather than blocking, and their results are ignored. And the
pipeline cache is *not* read back from a dead device: the read fails, and
writing the truncated result would poison the file for the next run.

**A rebuild that gives up half way has to say so.** The loop's only question is
`RENDERER(a)->lost(a)`, asked again the moment `recover_from_loss()` returns. On
a device loss that is free, since `device_lost` stays true until the last line.
On a surface loss it has to be arranged: once the surface is replaced its flag
comes down, and the device's goes up for the rest of the rebuild, so that both
clear together when it finishes. With neither raised, a
`create_device_objects()` that fails for a reason of its own reads to the loop
as "rebuilt" — `on_gpu_created` delivered against a device that has just been
nulled, and the next frame walking into `vkWaitForFences` on a null fence.

`App::cb_gpu_created` pairs the two callbacks: `on_gpu_lost` arrives once for
the loss, not once per rebuild attempt, and `on_gpu_created` only when there is
a device to hand back. Without it a driver stuck in a reset for a second calls
`on_gpu_lost` five times over, on a caller that has already destroyed its
pipelines. `wait_idle()` exists in the renderer seam for the same contract:
joining the render thread ends the CPU side and says nothing about the queue, so
a caller destroying pipelines in `on_gpu_lost` would be destroying objects a
command buffer may still be executing.

### Moving to another GPU

A window can be moved onto another GPU while it stays open
(`fnl_request_gpu()`, and the titlebar's GPU box). **The move is the loss
path, raised on purpose.** Everything a loss rebuilds is everything a new
GPU needs, every renderer already re-picks its device at that rebuild, and
the caller's half is already bracketed by `on_gpu_lost` and
`on_gpu_created` -- so a move is a wish and one flag: the render loop spends
the wish at the top of a turn (`gpu_switch_owed()`), sets `gpu_index` to
the GPU's place in its renderer's own list (`GpuEntry::local` + 1), raises
`surface_lost`, and the next question the loop asks is `lost()`. A path of
its own would be a second rebuild to keep in step with the first.

The surface goes with the device, because a driver leaves state of its own
on the window through it. Measured on Wayland: Mesa's WSI binds the
`wl_surface` to explicit sync the first time RADV sets the surface up, and
lavapipe's swapchain on the same `VkSurfaceKHR` then commits a `wl_shm`
buffer there -- `wp_linux_drm_syncobj_surface_v1` error 2,
`unsupported_buffer`, and the window gone. With the surface made again the
move runs both ways; on X11 it ran both ways either way.

**On Win32 a move presents into a window of its own.** A driver can claim
the window itself, not only the surface: aspire's Intel Vulkan driver
(31.0.101.2111, HD 530) sets a pixel format on the HWND -- 0 before it drew,
7 after, read with `GetPixelFormat()` -- and a window takes one pixel format
for its life, so NVIDIA's `vkCreateSwapchainKHR` on that window is
`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR` however long it waits and whatever is
made again: ten tries a tenth of a second apart, and a new `VkInstance`,
each refused the same. NVIDIA to Intel ran, and schenker's HD 630 sets no
format that stands in NVIDIA's way; Direct3D 12 made the same move on the
same machine. So from the first move the frames go into a child window
filling the client area, made again at every move by the window's thread
(`present_window_renew` in the backend seam, which the renderer's recovery
calls between the surfaces): a fresh HWND has no driver's format on it. The
child passes every hit test to the window (`HTTRANSPARENT`), so input, the
cursor and the zones are the window's as before, and follows its client
area in `on_size()`, before the size is published. Not from the start:
everything measured on Windows was measured on the window itself, and a
window nobody moves keeps presenting there. The sizes after a move are the
child's (`sizes_after_move()` in `stress.py`, on the first GPU moved to):
fullscreen and back, maximize and restore, and the border carried out with
the frame looked at, on both laptops' Windows, Direct3D 12 and Vulkan on
each GPU, all held.

**Under wine on Wayland the child is there from the start.** Wine 11.20
does not let
a window's surface go with its swapchain: `vkDestroySurfaceKHR` parks it
(`use_window_client_surface(FALSE)` in `win32u`) for the window's next one,
attached and still showing its last frame, until the window is destroyed.
Its Wayland driver puts each client subsurface directly above the window's
own surface (`wl_subsurface_place_above()` the parent), so a child made at
a move goes *under* that frame. Under cage the round lost the window after
the first move on all three bare machines: wine was handing the pointer to
the window (`WM_MOUSEMOVE` on the toplevel, traced with `+msg`), but the
child's surface took five presents and then no more -- wlroots sends no
frame callback to a covered surface, so the present blocked, and the
events waiting on the next frame were never drained. Weston sends the
callbacks, and the round passed there over a frame that was most likely
frozen. So under wine's Wayland driver `win32_open()` makes the child
before the window is shown, the window itself is never presented into, and
each move destroys the child it leaves, which is what frees its surface.
Not under its X11 driver: there a child is drawn offscreen and copied into
the window (`X11DRV_client_surface_present()`), and with the child from
the start every bare machine's Xorg read the window 100% black, Composite,
Present and DRI3 all there; XWayland on this desktop read it 0% black.
The difference is the client's, not the server's: wine there had no
`libXcomposite.so.1` to load, so winex11 never redirected the child and
its copy read nothing, and after a move the window kept the frame the
move left for good (*Measured across a lift*, below). With the library
in the bare machines' set a child shows there too; the window is still
presented into until a move, since a child under winex11 is a copy at
every present that the window itself does not need.

**On Wayland a move presents into a subsurface of its own.** A window
server can claim a surface's frames as well. Measured on GNOME Shell 50.1:
a window moved from RADV -- or from radeonsi, with no explicit sync on the
surface -- to lavapipe keeps showing the last hardware frame for the rest of
its life. Every lavapipe frame is reported presented and its buffers
released; none reaches the screen, and the way back to RADV is not shown
either. lavapipe on a fresh window draws, and so does the same move with
Mesa's commit timing off (`MESA_VK_WSI_DEBUG=nowlts`): it is the timed
commits of a `wl_shm` swapchain on a surface that had hardware frames. The
harness did not see it, since its round reads the echo and not the screen.
So from the first move the frames go into a desynchronized subsurface, made
again at every move (`wl_present_window_renew()`), as Win32's go into a child
window. The window's own surface keeps the role, the input, the outputs it
is on, the pointer lock and the fractional scale: its buffer is one
transparent `wl_shm` pixel its viewport stretches over the whole surface,
and the subsurface has an empty input region and a viewport of its own,
following the window's. What rides on the window's commit -- the ack, the
geometry, the viewport -- is committed after the present (`wl_flush()`): the
subsurface's frame is shown by its own commit, so the ack follows the frame
that answers it. The frame callback is the subsurface's, which the driver
commits. Without a viewporter the pixel cannot cover the window and the
frames stay where they were, said once. Measured: Vulkan on RADV to lavapipe
and back, to OpenGL and back, and OpenGL to lavapipe, each shown at once.

Driven (2026-10-22): `x11-vulkan` and `wayland-vulkan` between this
desktop's RX 590 and lavapipe, and `win64-d3d12-native` on both laptops,
each GPU in turn, through all three of its list -- the NVIDIA GPU, the
Intel one and WARP -- and back: 87, 66, 78 and 77 checks, none failed.
`x11-vulkan-bare` on both laptops' live keys, from the NVIDIA GPU through
the Intel one and lavapipe and back: 76 checks each, none failed; and
`wayland-vulkan-bare` there on weston, the same moves, 55 checks each,
none failed -- lavapipe's `wl_shm` frames after a hardware GPU's included,
on a surface made again.

**And the two surfaces present differently on weston, measured.** On
schenker's weston (the HD 630 driving the panel, the 1050 Ti drawing,
NVIDIA's 580.196.07, Vulkan, 60 Hz), a window opened on the 1050 Ti
presents into its own surface and one moved there presents into the
subsurface, and the benchmark's figures differ with it -- which is why a
run that measured the GPU ranking first (the ranking moves the window
through every GPU) read clean and one that opened on a kept ranking did
not, harness or not. Five seconds each, `fnl_frame_shown()`'s stamps:

| the 1050 Ti on weston | rate | missed refreshes | shown interval p99 | frame on screen p50 |
|---|---|---|---|---|
| opened there, its own surface | 59.2-59.6 fps | 3 | 33.31 ms | 13.6 ms |
| moved there, the subsurface | 60.0 fps | 0 | 16.67 ms | 21.2 ms |

The callbacks' intervals follow: p99 about 22 ms against 16.9. Neither is
simply better -- the window's own surface misses about one refresh in a
hundred, and the subsurface misses none and shows each frame some 7.5 ms
later. The Intel GPU, which drives the panel, is clean on its own surface
(p99 16.9 ms, 300 frames), so it is the frames that cross from one GPU to
the other; turning off the frame-stats counting and restarting weston
changed nothing.

**The own surface's misses are the client's commits, not weston's
repaint** (2026-10-30, weston's `timeline` and `drm-backend` debug
scopes beside a 10 s measure, 6 refreshes in about 600 with no new
frame). weston puts the window's buffer straight on an overlay plane of
the Intel display in every one of its 957 repaints -- only the background
goes through its renderer -- and its repaint, begun to posted, is about
a millisecond; it leaves its repaint loop 6.7 ms after a vblank when
nothing is committed, and a commit up to about 15 ms after the vblank
still reaches the next one. Each miss is a period with no commit at all:
the frame's pace (the late-latch hold and the wait for the frame
callback, 13.5 ms median) ran to 16 ms, and NVIDIA's
`vkQueuePresentKHR`, which is the PRIME copy, took up to 5 ms against
its 2.8 median, so the commit landed after the vblank it was for (21.5
ms after the one before, in the first).

**And the subsurface's later frames are the client's too.** weston puts
the subsurface's buffer on the same overlay plane and takes its commits
at the same 6.7 ms after the vblank -- steadier, p95 6.77 ms against the
own surface's 10.67 -- so what it adds is in the frame before the
commit: NVIDIA's swapchain on the subsurface makes the acquire wait for
an image (1.75 ms median, against 0.01 on the own surface), the late
latch counts that wait as the frame's cost and learns a shorter hold
(7.9 ms against 9.7), and the state is latched earlier by both: 18.5 ms
from latch to screen against 14.8 in the same session. Why NVIDIA's
driver holds the subsurface's images longer is its own.

**cage shows the same** (wlroots, on the same machine and GPU, 10 s
each, 2026-10-30):

| cage, the 1050 Ti | rate | missed | shown p99 | latch to screen p50 |
|---|---|---|---|---|
| opened there, its own surface | 59.4 fps | 2 of 594 | 33.3 ms | 17.7 ms |
| moved there, the subsurface | 60.0 fps | 0 of 600 | 16.67 ms | 28.6 ms |
| the HD 630, which drives the panel | 60.0 fps | 0 of 600 | 16.67 ms | 19.1 ms |

**So the window keeps opening on its own surface.** On two compositors
the own surface misses a refresh in one or two hundred where the GPU
drawing does not drive the panel, and is clean where it does; the
subsurface misses none and costs 4 to 11 ms on every frame. Presenting
into a subsurface from the start would pay that on every frame of every
window to save a rare miss that only a GPU across PRIME shows, and the
library does not yet know which GPU drives the panel. GNOME is the one
compositor of the three not measured so (TODO.md).

**The list is the pick's, and the index is its order.** Each renderer's
pick already walks every device it could draw on and logs a line per device
(*The startup inventory*); the same walk fills `App::gpu_list`
(`app_gpu_list_publish()`), so the list a caller reads, the `GPU n:` lines
and the numbering `gpu_index` and `FNL_GPU` count by cannot disagree. Each
entry carries its place in its renderer's part (`GpuEntry::local`), which is
what makes the move one assignment.
Vulkan lists the devices that can present to the surface, lavapipe
included where its ICD is installed; Direct3D 12 the adapters that make a
feature level 11_0 device, WARP among them; Metal every `MTLDevice`. The
kind comes from the API: Vulkan's device type, Metal's unified memory and
`removable`, and for Direct3D 12 software or not -- DXGI says nothing of
integrated and discrete short of a device per adapter at every rebuild, and
a guess from the memory it reports is wrong on APUs.

**OpenGL lists what its drivers will open this window on.** EGL lists its
devices (`EGL_EXT_device_enumeration`), and a display can be had on one: on
the device itself where there is no window (`EGL_EXT_platform_device`,
KMS), and on the window server with the device named
(`EGL_EXT_explicit_device`). Not every driver takes the name. Measured on
schenker (Mesa 26.2.5, NVIDIA 580, `tools/probe/eglexplicit.c`): on X11
both draw on the server's GPU whatever is named, and NVIDIA's refuses the
xcb display it is named for; on weston NVIDIA's takes the name and Mesa's
does not -- its software device too, which `gl_bridge_context()`'s note
already had. So a device is listed where a display opened for this window
on it says it is on it (the display's `EGL_DEVICE_EXT`), once, at the
first open (`list_devices()`), each such display opened and closed again:
on KMS every GPU (the NVIDIA one, Intel's, llvmpipe), on weston the NVIDIA
one beside the compositor's, on X11 the server's alone. A device a driver
views but has no driver for -- Mesa's of the NVIDIA card under NVIDIA's own
module -- has no name to give and is passed over unopened. A move closes
the display and opens it on the new device, the native window made again
over it. With no GPU named the display is the window server's choice, or
on KMS glvnd's: NVIDIA's EGL, first by its vendor file, answers the
surfaceless display. A device whose display opens and makes no 4.5 core
context (`makes_gl45()`, asked with no configuration) is left out with a
line: nouveau on aspire's GM107 and schenker's GP107 (Mesa 26.2.5)
answers `EGL_BAD_MATCH`, and listed, a move to it fails and comes back.
The way back is a move as the way there was, the surface's loss raised
again (`device_recover()`): from the device's loss alone OpenGL recovers
on the display that failed, and does so every turn. Forced
(`FNL_FAULT=recover-objects@1`, the gc tree): on OpenGL, KMS on aspire,
the move to llvmpipe fails, one line says so, and the window is rebuilt
on Intel's and drawn there to the end of the round; on Vulkan, X11 here,
the same from llvmpipe back to RADV, at the size it had; on Direct3D 12,
both laptops' Windows, from the NVIDIA GPU back to Intel's and from
Intel's back to the NVIDIA one, the window drawn there to the end of the
round.
Mesa's surfaceless display, with no NVIDIA driver to answer it, takes the
first render node, which there is nouveau's, so EGL's own choice gives
way to the first listed GPU that is not software where it is one left
out; where nothing is listed the context fails and says how to get
llvmpipe, as before.

**On KMS the list is every GPU with a graphics queue, and each reaches the
output one of three ways.** The surface there is the panel's display, which
one device holds, so a list of the devices that can present to it would be
that device alone. Instead the bridge answers for each device
(`VkBridge::can_draw`), and `pick_device()` lists and numbers those, the
KMS surface reading an index by the same count (`wanted_device()`). A move
lets the held display go (`let_display_go()`) and decides the way afresh:

* the new device takes the display, where it will;
* else the copy path on a headless surface, where the device presents to
  one -- Intel's, lavapipe, NVK;
* else the copy path with **no surface at all** (`RendererState::
  surfaceless`): the window is two images of the renderer's own, taken in
  turn where a swapchain's would be acquired, with no semaphores, no
  present and the window pass ending in the copy's layout, the readback
  being the present. NVIDIA's own driver (580) needs this: beside an Intel
  panel it names no display (`vkGetDrmDisplayEXT`, `VK_ERROR_UNKNOWN`) and
  has no `VK_EXT_headless_surface`.

The headless surface stays ahead of no surface because it is the swapchain
every other backend has, the one the rest of the renderer was measured on;
the surfaceless window is for the driver that leaves no other way. The
backend's half (`present_window_renew`) has the copy path's next frame set
its mode again, since the display path set the CRTC its own way in
between; the pointer's plane needed nothing, measured whole after a move
each way with nothing done for it. Measured on aspire and schenker: the
Intel GPU on the display, the NVIDIA GPU with no surface, lavapipe on a
headless one, and back. And with no surface for every GPU, forced
(`FNL_FAULT=kms-no-display,kms-no-headless`, the gc tree), on both
laptops booted with nouveau: Intel's, NVK's and llvmpipe each draw into
images of their own, the pointer drawn after every move. And the copy path
on either side of the display (`FNL_GPU=nvidia`, NVIDIA's own driver):
NVIDIA's images of its own, Intel's on the display, llvmpipe's headless
surface, NVIDIA's again, on both laptops, the mode set afresh by each copy
path after the display's and the pointer drawn after every move.

**A GPU that will not build is gone back from, once.** The loop keeps the
choice the old GPU was made by; when the rebuild on the new one leaves the
loss flag up, it puts that choice back and lets the next turn rebuild on
the old GPU, with one line saying why. A second failure is an ordinary loss
from there on. The move is said once, as `moving to GPU n: <name>`, and the
rebuild it raises is neither logged as a loss nor slept on: nothing has
been reset that a pause would wait out.

**`FNL_EVENT_GPU` is diffed, not posted.** `events_post_state()` compares
the GPU in use with the one last reported, as every other notification is
compared (*The event queue*) -- by name, renderer and place in the list,
since a list ordered again makes a caller's index stale, and not by a count
of changes, which a move that went back counts twice; and not while the veil
is up. A rebuild that lands on the same GPU, after a
loss or a failed move, posts nothing.

**The round moves it where there is a choice** (`gpu_moves()` in
`stress.py`): the list and the GPU in use off the inventory the library
logs at every pick, then the box onto each other GPU and back, each move
held to its `EV gpu` (the index and the renderer), a `moving to GPU n`
line with no loss beside it, and the window drawn at its size after.

### Moving to another renderer

**The list spans every renderer the caller has a scene for.** Each entry
carries the renderer that listed it, so the one list holds Vulkan's GPUs
and OpenGL's on Linux, Direct3D 12's and Vulkan's on Windows, Metal's and
MoltenVK's on a Mac, and a move to one of another renderer's is a move to
that renderer. `FnlInitDesc::renderers` says which renderers those are,
where `FnlInitDesc::renderer` names none: a caller that names one has
that scene alone, and its list is that renderer's whatever the mask says
(`renderer_scenes()`, read in `renderer_candidates()`'s order). And
`FNL_RENDERER` names only the one the window starts with: the list
still spans the rest, which is what lets every round of the harness, each
of which names its renderer, move across.

**The window's renderer first, then the others in the order tried, for the
window's life** -- unless a ranking is kept, which orders the list by the
display's figures (*Ranked by measured frame rate*). `gpu_index` and `FNL_GPU`
count the starting renderer's own list, so they count this one; an index names
the same GPU across a move, so `FNL_EVENT_GPU`'s index and the titlebar's rows
mean what they meant. Each part is its renderer's: the drawing one's from its
own pick (`app_gpu_list_publish()`), the others' from a seam entry, `list_gpus`,
that lists without building -- the same entries in the same order as the pick,
so that an index into it is the `gpu_index` the pick will take, and with no
state of that renderer's, since `App::renderer_state` is the drawing one's.
Vulkan makes an instance of its own in a scratch state of its own, never
`App::renderer_state`, and asks each device whether a queue presents to the
window server with no surface (`VkBridge::presents`,
`vkGetPhysicalDevice*PresentationSupport KHR`; KMS's `can_draw`, a Mac's every
device); OpenGL lists with the same code its own open uses (`list_devices()`,
into a `GlDevices` of the caller's), and where EGL lists nothing it names the
window server's one GPU by a context made current on no surface; Direct3D 12 and
Metal enumerate as their picks do. The inventory names the renderer beside the
kind wherever the caller has scenes for more than one, and numbers each line as
the whole list counts (`app_gpu_inventory()`).

**The others are listed after the first frame, on a thread of their own,
and timed.** Listing loads their drivers -- an instance of Vulkan's, a
display of EGL's per device -- which the window's first frame has no need
to wait for, and which on the render thread held the frame after it as
long; the line `<renderer> lists n GPUs here, in t ms` is the cost, said
where it is paid. The thread (`app_gpu_list_start()`) is joined with the
render thread. Nothing it calls reads `App::renderer_state`: Vulkan's
instance is made in a state of its own, which `make_instance()` and the
bridges' `presents` are handed, and the list is published only once that
instance is gone, so no move to Vulkan can start beside it. Measured:
OpenGL's one GPU on this desktop 56 ms, its two or three on the laptops'
Linux 41 to 89 ms; Vulkan's three there 36 to 54 ms; on the laptops'
Windows Vulkan's two 194 to 398 ms and Direct3D 12's three 14 to 456 ms;
on the Mac Vulkan's one 30 ms and
Metal's 0. Deciding the renderer before `open()` instead would cost a
refused renderer's build before the window exists, about one listing
more for each one tried (2026-10-31, aspire's console, a probe of the
loader and `libEGL` alone, each cold in a fresh process): a Vulkan
instance with the surface extensions, its devices enumerated and a
device made, 71 to 105 ms on NVIDIA's driver and 83 to 137 on nouveau,
where NVK wakes the GPU to enumerate it; an EGL display and a 4.5 core
context made current, 44 to 55 ms. The first cold instance after the
machine sat idle is a third over the rest. The list growing is an
`FNL_EVENT_GPU`, as a move is: the
event says to read the list again, and its length is compared beside the
GPU in use.

**And off the render thread for a second reason: its context.** Listed
there, Vulkan's drivers -- NVIDIA's 580 among them -- left OpenGL's
context not current: on NVIDIA's EGL on KMS nothing more was drawn and the
thread spun, on Mesa's under weston the next swap failed as a lost
surface. A context is current per thread, and the listing's is not the
render thread's. Where the thread will not start, the listing runs on the
render thread and the renderer is entered again after it (`thread_enter`),
which for OpenGL makes its context current.

**NVIDIA's Vulkan driver terminates NVIDIA's EGL display when an instance
beside it ends.** The fuller finding, with `tools/probe/eglmove.c`'s
held-context steps on aspire (580.196.07, glvnd 1.7.0, no window server):
a display on NVIDIA's device, a context made current and released, a
Vulkan instance made and destroyed, and every `eglMakeCurrent` after it
`EGL_NOT_INITIALIZED`, on the thread that held the context and on a new
one alike; the display opened before or after the instance, glvnd closed
and opened again in between or not; and nothing of it with Mesa's drivers
alone, or with the instance before the display. A context already current
survives a terminated display until it is released, which is all that the
listing's own thread bought: the frames went on, and the main thread's
`eglMakeCurrent` at the end failed. So where OpenGL is the renderer to
draw, the others are listed before its display opens, as under wine
(below; 114 ms for Vulkan's three on aspire, 120 ms on schenker's KMS --
and 8.6 s there at the first start after a boot, NVIDIA's driver
initialising the GPU, which the window's first frame then waits out);
the listing is not run
again on the way back from a move that missed, where the list stands
(*Another kernel driver*); and the OpenGL renderer reads
`EGL_NOT_INITIALIZED` from a display it initialised as the display
terminated under it (`RendererState::display_gone`) -- a caller's own
Vulkan instance would do it -- and makes everything again from the display
up at the next recovery, where it retried a dead surface for ever.

**Except under wine, and where OpenGL draws, where they are listed before
the one drawing is built.** There Direct3D 12 is vkd3d-proton on the same
Vulkan driver, so the listing is a second Vulkan instance in the process
beside a swapchain that presents (`FnlBackendVtbl::shared_driver`). On
NVIDIA's 580 under weston and cage, both laptops, the window's Direct3D 12
program aborted at
startup (exit code 3, no line): with `WINEDEBUG=+seh`, vkd3d-proton's
present-wait thread called a null pointer inside `vkWaitForPresentKHR`
within the 37 ms the Vulkan instance was being listed and destroyed.
Wine's own table for that call is per device, so the null is the
driver's. Not on Xorg, and not on lavapipe. So `fnl_init()` lists every
other renderer the list spans before it builds each candidate
(`gpu_list_early()`), and the thread after the first frame publishes what
they said; the cost is paid at startup there, 91 ms for Vulkan's three on
aspire.

**A move to another renderer is a swap on the render thread.** The loop
raises it as any move (`gpu_switch_owed()`, `App::renderer_next`), the
caller is told `on_gpu_lost` while the old renderer still answers
`fnl_renderer()`, and then `renderer_swap()` tears the old one down as
`app_teardown()` would -- device objects, surface, instance -- renews the
backend's present window, as a move between two GPUs of one renderer
does, and builds the next with `renderer_build()`, the build `fnl_init()`
uses, on this thread, which then holds it. `on_gpu_created` follows, and
the caller asks `fnl_renderer()` which scene to build -- the testbed
already did, at every one. Where the next will not
build, the one before is built again in the same turn, on the GPU it had,
with one line (`renderer-move-refused` forces it); where that will not
either, `App::renderer_owed` holds it, nothing below the loop's top is
asked of a renderer that is not there, and the build is tried each turn.

**Driven (2026-10-23), every move across and back in each list.** This
desktop: X11 and Wayland, each started on Vulkan and on OpenGL, release
and under ASan, TSan and the `gc` tree (98, 73, 99 and 74 checks, none
failed), and wine on X11 and on Wayland from Vulkan and from Direct3D 12.
Both laptops' Linux: X11, weston and KMS with each renderer (87, 65 and
62 checks), KMS and weston under ASan and TSan. Both laptops' Windows,
each GPU, from Direct3D 12 and from Vulkan: five GPUs listed, 91 and 92
checks. The guests, WARP and lavapipe. The Mac: Metal to Vulkan on
MoltenVK and back, on one GPU, 77 checks. What is not
moved to: Direct3D 12 under wine's own vkd3d, which makes the device and
refuses every DXIL pipeline state. Each adapter list -- the pick's and
`list_gpus`' alike -- builds the rectangle's pipeline state on a device
made for the question and leaves out an adapter where it is refused,
with a line (`GPU left out: ..., whose Direct3D 12 builds none of the
library's pipeline states`) -- under wine alone, since on Windows a
driver runs DXIL to be a Direct3D 12 driver at all, and the build cost
each list some 100 ms there (22 to 25 ms without it, 122 to 261 with,
on both laptops' three adapters) -- so plain wine lists no Direct3D 12
GPU at all: no entry in the GPU box that comes back, and none for the ranking to
measure as nothing on every display (2026-10-31, this desktop: before, an
"ATI Radeon HD 5600 Series" entry, wine's name, measured 0 on each of
three displays). The harness's `NO_MOVE_TO` stays for a wine that lists
one; the d3d12 environments load vkd3d-proton, where the adapter builds
and is listed, and move both ways. And NVIDIA's own driver under TSan,
which a round does not start on either. The way back, forced
(`renderer-move-refused@1`, the gc tree, X11 here): `OpenGL would not
draw here …; back to Vulkan`, and the window drawn on with Vulkan on the
GPU it had to the end of the round.

**Under the checkers a swap unloads drivers.** The loader unloads an ICD
with the last instance, and a swap destroys it. Lavapipe then leaves two
56-byte blocks its globals held, which LSan reports from an unknown
module, and its fence teardown races its own rasteriser threads
(`lp_fence_destroy()`, by its debug info) -- what the bare machines
already met when a round ended there. This desktop's asan rounds keep it
loaded as theirs do (`LD_PRELOAD`), and its tsan rounds read
`tsan-lavapipe.supp` beside `tsan.supp`; GCC's symbolizer does not read
Ubuntu's `.dwz`-split debug info, so a key on the function is not to be
had here.

**What other threads read of the renderer is published.** `App::renderer`
was set once before the render thread started; a swap writes it while
that thread runs. A reader on another thread -- `fnl_renderer()`,
`fnl_max_samples()`, `fnl_present_mode()`, the titlebar's checkboxes from
a press, Win32's move handler -- takes `App::renderer_pub` instead
(`renderer_any()`), which names a table that is static. Nothing such a
reader then calls reaches the renderer's state: the sample count, the
present mode and the GPU's name are App's, which each renderer writes.

### Another kernel driver, with the window open

An NVIDIA GPU on Linux has two kernel drivers, nouveau (under Mesa's NVK)
and NVIDIA's own module, and the kernel moves it from one to the other
with no reboot: the one module unloaded, the other loaded by name.
Measured on schenker (GTX 1050 Ti, Arch's 7.2.11, NVIDIA 580.196.07, the
live stick's NVIDIA entry): nvidia to nouveau in 0.85 to 0.9 s and back in
1.9 s, four times each way, nothing in the kernel's log; from a GPU
nouveau had runtime-suspended as well; no reset between, which a GPU
before Turing has no GSP firmware to want. The other driver's packages
blacklist it, and the boot entry puts it on `modprobe.blacklist`; neither
stops a module loaded by name, only one loaded by alias.
`nvidia_drm.modeset=1` and `fbdev=1`, from the kernel's command line, are
in force again at each load, and the console stays on Intel's
framebuffer. A process holding a device node of the GPU stops the move
cleanly: modprobe says "in use" and nothing is taken.

**Each driver lets the GPU go its own way.** NVIDIA's module stays loaded
and the GPU is unbound from it through sysfs, and bound again the same way
(`driver_override`, then `bind`); nouveau is loaded and unloaded, by
`rmmod`, which unloads nouveau alone -- `modprobe -r` also unloads what
it alone used (`drm_gpuvm`, `drm_exec`, `gpu-sched`, `mxm-wmi`), which the
next move onto it loads again. Loading
NVIDIA's module was 1.7 s of the 1.9 s a move onto it took -- 90 MB,
compressed -- and bound again it takes 0.4 s: the move to nouveau is 0.52
s on schenker (1.16 on aspire, whose nouveau loads slower) and the move
back 0.55 s, where they were 0.85 and 1.9. Measured with something
holding the GPU open, a live Vulkan device on it:

* NVIDIA's unbind waits. "Attempting to remove device with non-zero usage
  count", and the write sleeps until the holder closes, the holder's
  device working as before meanwhile, then completes, and the GPU binds
  again and draws. A race lost is a stall, not damage.
* **nouveau's unbind panics the kernel** (7.2.11, a page fault in
  `nve0_bo_move_copy` under `nouveau_ttm_fini`, evicting the open
  device's buffers; schenker's `efi_pstore`). Its `modprobe -r` refuses
  instead, atomically, while anything holds the module.

So nouveau is never unbound, and the holders are looked for before
NVIDIA's unbind. That also settles the question of a move without root:
sysfs's `bind`, `unbind` and `driver_override` check nothing but the
file's mode, and handed to a group they would move the GPU unprivileged
(measured, 129 ms), but they would hand that group nouveau's unbind, a
kernel panic a check before it cannot rule out; and unloading a module
wants root. The helper stays root, behind a socket (below).

**Not a kernel module.** One of our own could not do the nouveau half
better: unloading is the `delete_module` system call and nothing a module
may call (it is not exported, nor is `kallsyms_lookup_name`, on 7.0), so a
module would unbind nouveau -- the panic -- or start `modprobe` as a root
process, which is the helper again. The one atomic gate there is, "no
holder and no new one", is that call's: it counts every open node, every
descriptor in flight between processes and every buffer another driver
imported (a dma-buf holds its exporter's module), where a scan of
`/proc/*/fd` sees only the first. What a module would add is the
privilege, which a root service gives as well, without a build for each
kernel, a signature for Secure Boot, or a crash in the kernel for a bug in
it. A patch to nouveau, refusing an unbind while a client is open, is the
change that would let nouveau stay loaded as NVIDIA's module does, and is
not ours to carry.

**One GPU, and the move never cut short.** A module unloaded is unloaded
from every GPU it drives, and which of two NVIDIA GPUs a name picks after
the move is the driver's order; so the move is offered only where one GPU
can make it, and two are said and left alone. A helper killed between the
unload and the load would leave the GPU with no driver and skip its own
way back, so the library waits for a move to its end -- every wait in the
helper is bounded but the kernel's unload and NVIDIA's unbind -- and says
once that it is slow. Nor may a Ctrl-C at the terminal kill it, which
reaches the program's process group: the helper runs in a group of its own
and, from its lock on, ignores hangup, interrupt, terminate and a closed
pipe, in what it runs as well -- measured, SIGINT to the program's group as
it asked for a move, the program gone and the GPU on nouveau with its
nodes. SIGKILL is left, and a process the program started dies with the
program's session however it is grouped; so the library asks a service
instead, which is no process of the program's: systemd's socket
`/run/fenestal-gpu-driver.sock`, which the group `fenestal-gpu` may
connect to, a service a connection (`Accept=yes`) running the helper's
`serve`, which reads one request as a line, checks its words before
anything runs as root on them, and writes `status: <n>` after the
helper's lines. A move there outlives the program, and the service killed
halfway has an `OnFailure` unit, the helper's `rescue`, that binds a
driver to an NVIDIA GPU left on none: NVIDIA's module where it is loaded,
0.4 s, and nouveau where that one will not bind. A move whose new driver
will not bind and whose old one will not bind again -- or that started
from none -- does the same before it fails, since that service ends well
and its `OnFailure` never runs; where that second try binds the driver
asked for, with its nodes, the move is made and says so.
Where there is no socket, or the account is not in its group yet,
the library goes through `sudo -n` as before, and says so. A move cut
short anyway, the GPU left on no driver, is put back like one that
missed. Whether a move
landed is read off the list made again after it, not off the helper's
status, which a caller that reaps its own children takes. NVIDIA's
persistence daemon holds the GPU open by design: the helper stops it
before it looks for holders, and starts it again where the move is
refused or where it is enabled and NVIDIA's module is loaded again.

`tools/gpu-driver.sh` is the move, as root: only an NVIDIA display device
bound to one of the two or to nothing, named by an address the helper
itself lists -- the sudoers rule passes any words, and a directory of the
account's own with a `vendor` and a `class` would have had root write
through its symlinks (measured on aspire, a file truncated) -- callers
taking turns on a lock in `/run` that only root may open, where one in a
world-readable file is a lock any account may hold; the
holders looked for first and named (`held: <pid> <name>`, status 2) -- on
nouveau its DRM nodes alone, since a program with NVIDIA's userspace
loaded holds `/dev/nvidiactl` and nothing of the GPU -- and named again
every three seconds while NVIDIA's unbind waits for one that came after
the look, which the library logs as each line arrives; the GPU let go and
taken, and its nodes made by udev and given their group before it
returns, waited for by name rather than by `udevadm settle`, which waits
for every event the machine has queued. It says how long each step took
(`let go`, `bound`, `nodes`). After a move onto NVIDIA's module it loads
nouveau beside it, the GPU kept off it by `driver_override`, so that the
next move onto nouveau binds and does not load; after answering, never in
the move's way. The harness sends it over root ssh
(`kms-vulkan-bare@machine:nouveau`, `bare_driver.py --driver`); `--setup`
installs it as `/usr/local/sbin/fenestal-gpu-driver` with the socket's
three units, the group, and a sudoers rule for the account. The list the
library offers moves from is sysfs's, which anyone may read, and runs
nothing (`src/backend/kms/kms_driver.c`).

**The KMS backend's alone.** A move needs the GPU held by nothing, and a
window server can hold it where no client can let go: Xorg keeps
NVIDIA's open through its X driver for as long as it runs, and wlroots,
cage's library, opens every card. On KMS the program is the machine's only
display client -- the panel's card (Intel's) is its own, and the NVIDIA
card it closes at open when it has no output to give -- so the renderer is
all that holds the GPU. Where the NVIDIA card *is* the display's -- the
machine's one GPU, or `FNL_KMS_OUTPUT` naming an output wired to it -- the
backend holds it for the window's life, and lists no move at all, said
once at start, rather than offer one the helper refuses each time. Under a
window server the move is refused with a line, and the other driver's boot
entry is the way there, as it is for a round on Xorg, weston or cage.

**The window stays; the renderer is built again.** What holds the GPU inside
the process is the renderer's, and a Vulkan instance destroyed lets it go,
every descriptor of both drivers (measured with a probe that reads
`/proc/self/fd` around `vkCreateInstance` and `vkDestroyInstance`). So the
move is a renderer swap (`renderer_swap()`) onto the renderer the list's
entry names, the driver changed between the teardown and the build, where
nothing of the renderer exists; the window, its surface's window and the
caller's state are kept, the window is drawn on another GPU while the move
runs where it can be (*Drawn across the move*, below) and the frames pause
for it where it cannot, and the caller's half is bracketed by `on_gpu_lost`
and `on_gpu_created` as any move is. The listing thread's instance is joined
first. A refusal -- the GPU held -- is the swap's own way back, onto the GPU
and renderer before. A move that lands elsewhere -- the build's pick matched
no GPU of the moved one's name and took another, or the build failed -- is
no move: the renderer is torn down, the GPU put back on the driver it left
(`app_gpu_driver_undo()`), and the window built again where it was: on the
renderer it was on before the move, by the choice that put it there
(`App::gpu_cross_from`, `gpu_back_*`), which is not the one it was drawn
across on -- a choice is an index into one renderer's list.

**OpenGL, and glvnd closed between its spells.** glvnd loads NVIDIA's EGL
beside Mesa's for every display, and while it is loaded NVIDIA's driver in
this process is fixed as it was. On NVIDIA's module its first device query
opens `/dev/nvidia0` and `nvidiactl`, and a display of its own keeps more,
for as long as the library stays, `eglTerminate()` and
`eglReleaseThread()` or not: the move is refused with this process named.
Loaded with the GPU on nouveau it holds `nvidiactl`, and after a move
NVIDIA's Vulkan driver here does not see the GPU -- it finds its GPU
through glvnd's vendors -- so the build drew elsewhere and the way back
was refused, the GPU left on NVIDIA's module. Neither is NVIDIA's EGL's
own doing alone: the Vulkan loader closes NVIDIA's ICD at
`vkDestroyInstance`, and glvnd beneath it, so each Vulkan instance starts
glvnd afresh, which is why a Vulkan window moves freely. So with
FNL_GPU_DRIVER on, the OpenGL renderer closes `libEGL.so.1` with its last
hold -- the instance's, and each listing's -- and opens it again for the
next (`src/renderer/gl/egl_dyn.c`, the one runtime the library closes).
Measured on schenker with the probe now `tools/probe/eglmove.c` (glvnd
1.7.0, Mesa 26.2.5, NVIDIA 580.196): NVIDIA's own OpenGL drawn, glvnd
closed, every descriptor gone, the GPU moved to nouveau and back, Vulkan
listing NVK and then NVIDIA's GPU, and NVIDIA's OpenGL drawn again after;
the same from nouveau with OpenGL on Intel; and with glvnd left open,
the device query alone, or a display on Mesa's surfaceless platform with
none, NVIDIA's Vulkan driver blind after the move. glvnd's destructor
drops its thread key and every thread's state, and Mesa's EGL keeps its
own in a plain thread-local, so a thread that called EGL runs no code of
either after the close; each thread hands its state back first
(`eglReleaseThread()`) all the same.

What is closed is what holds the driver, and not what took the static TLS
block. A library whose thread-locals live there (`DF_STATIC_TLS`:
libGLdispatch, Mesa's EGL and gallium, NVIDIA's libnvidia-tls) gets its
room from a surplus glibc reuses only from its end, and the Vulkan
loader's loads of NVIDIA's ICD in between leave each room freed in the
middle: on schenker OpenGL listed one GPU after the tenth move from
OpenGL and none after the eleventh, glvnd loading no vendor that needed
room. The same twenty moves passed with the surplus made large
(`GLIBC_TUNABLES=glibc.rtld.optional_static_tls`), which a library cannot
set: glibc reads it as the process starts. So before glvnd is closed every
library with static TLS that EGL's own spell brought in -- mapped at the
close and not at the open, found by `dl_iterate_phdr()` -- is kept for the
process's life (`RTLD_NOLOAD | RTLD_NODELETE`); none holds a GPU, Mesa's
letting go at `eglTerminate()`. Not the Vulkan loader's drivers, mapped at
a close whenever Vulkan draws while OpenGL's GPUs are listed: NVIDIA's ICD
has static TLS too, and kept it would stay for the process's life, where
the move rests on each instance starting NVIDIA's userspace afresh. The
loader loads and unloads its drivers as one, so they take their room at
the surplus's end and give it back there; twenty moves from OpenGL passed
with them left out, OpenGL listing its three GPUs after each.

**Measured, release tree, KMS** (2026-10-23, `drivermove 20 gl`: before
each move the window is put on OpenGL on the NVIDIA GPU, so every move
starts from OpenGL -- NVIDIA's own, or Zink's on NVK -- and lands on
Vulkan): twenty moves in one process on each laptop, OpenGL listing all
three GPUs after each. Schenker: to nouveau 0.84 s, 13 frames drawn
meanwhile, 0.57 to 0.59 s the longest without one; back 0.81 s, 36
frames, 0.13 to 0.16 s; onto NVIDIA's OpenGL after 0.14 s, onto Zink's
0.80 s. Aspire: to nouveau 1.44 s, 53 frames, 0.30 to 0.31 s; back 0.97
s, 32 frames, 0.29 to 0.33 s; onto NVIDIA's OpenGL 0.16 s, Zink's 0.41
s. `NOUVEAU_USE_ZINK` put OpenGL on NVK there as the probe's override
did.

Two ways round were measured and left. Mesa's vendor alone
(`__EGL_VENDOR_LIBRARY_FILENAMES` before libEGL is opened) hides the GPU
from NVIDIA's Vulkan driver for the process's life, the variable cleared
after EGL's start or not: glvnd reads its vendors once a load. Mesa's
vendor library driven directly (`libEGL_mesa.so.0`, its `__egl_Main`
handed a table of glvnd's callbacks, which only Mesa's dispatch stubs
read) works -- OpenGL 4.6 on Intel, both moves, Vulkan seeing each
driver's GPU after -- but gives up NVIDIA's own OpenGL, which closing
glvnd keeps.

What is checked instead of trusted: before a move, with the renderer torn
down and the listing joined, the process must hold no `/dev/nvidia*` and
nothing of the GPU's (`gpu_driver_leftover()`, `gpu_driver_held()` with
`every_nvidia`). Where it does -- glvnd a caller opened, say -- the move
is not made, said with the path, and no move is offered after it
(`app_gpu_drivers_pin()`); `FNL_FAULT=gpu-driver-leftover` forces it.
The descriptor says the driver is held and not by whom, so the line names
NVIDIA's libraries still mapped beside it, or that none is: a library
unloaded with its descriptor left open. And with the move entries gone
every entry behind them has a new number, so the list is said again.
NVML is such a holder: started, it opens `/dev/nvidiactl`, `nvidia0`,
`nvidia-uvm` and a capability node, and `nvmlShutdown()` closes them all
(schenker, 580.196). The testbed's F5 panel asks it for the process's
memory on NVIDIA's OpenGL and shuts it down at each `on_gpu_lost`
(`gpumem.c`); a caller that asks NVML anything wants the same, or the
move is refused.

On nouveau OpenGL draws through Zink: nouveau's own makes no 4.5 core
context on GM107 or GP107 (`EGL_BAD_MATCH`), and Zink on NVK makes one
and lets go with its display, as any Vulkan device does -- measured with
the probe, OpenGL 4.5 on NVK, glvnd closed, the move to NVIDIA's module
made and its GPU listed. FNL_GPU_DRIVER sets `NOUVEAU_USE_ZINK=1`, Mesa's
loader's switch for nouveau alone, where the environment has none: the
one variable the library writes, in `fnl_init()` before any thread or
renderer of its own. The other renderers' GPUs are listed again once a
move has landed, since a GPU's name and who can draw on it are its
driver's; never while one runs, and not on the way back from one that
missed, where the list stands and a listing's Vulkan instance beside
OpenGL's display would terminate it (*Moving to another renderer*). For
the same reason a move that lands on OpenGL lists them before OpenGL is
built on the GPU moved, as `fnl_init()` does for a window that starts on
OpenGL, and every landing drops what was listed so before it, which is
the old driver's (`gpu_relist_landing()`). The move entries are named by
Vulkan's GPU alone, whatever renderer they land on: that name is in each
renderer's -- NVIDIA's OpenGL puts "/PCIe/SSE2" behind it, Zink wraps it
("zink Vulkan 1.3(NVIDIA GeForce GTX 960M (NVK GM107) (MESA_NVK))") --
where OpenGL's own would not be found by Vulkan's pick.

**Asked for through the GPU list.** With FNL_GPU_DRIVER set each GPU that
can move is an entry more for each renderer the caller has a scene for,
`<GPU>, on <driver>` with that renderer, which `fnl_request_gpu()` and
the titlebar's GPU box take like any other, and drawn after the move on
that GPU with that renderer, by the name it keeps on either driver (NVK's
"(NVK GP107)" cut). With the GPU's own entries every renderer on every
driver is one entry of the list at once -- on schenker four for the GTX
1050 Ti, Vulkan and OpenGL on nvidia and on nouveau -- and each of the
GPU's rows in the GPU box names it alike, by that name and the driver
the row draws on ("NVIDIA GeForce GTX 1050 Ti on nouveau"), where its
renderers' own names differ. Each of the four keeps its number for the
window's life, as every entry of the list does across a move to another
renderer: the driver the GPU was on when a move was first offered has its
rows where the renderers list the GPU, and the other driver the move
entries at the end, so while the GPU is away each renderer's row of it
and that renderer's move entry, the way back, trade places
(`app_gpu_driver_entries()`). A row is then mapped back to its renderer
by where the entry came from, not by counting the parts, and the
inventory lines number the GPU as the box does (`gpu_number()`, app.c). Both
laptops' renderers list the NVIDIA GPU first on either driver; one that
listed it elsewhere on the other would move its row there with it.
Eight entries hold both laptops' lists,
three GPUs a renderer and the two moves (`GPU_LIST_MAX`). The window is
drawn across the move on Vulkan whatever it lands on (*Drawn across the
move*): an
OpenGL display loads NVIDIA's EGL, which holds the GPU; so where the
caller has no Vulkan scene the frames pause instead. A landing that finds
no GPU of the name withdraws that driver's entry for that renderer alone
(`app_gpu_driver_unseen()`): the renderers are each a userspace of their
own there, Zink on NVK beside NVK, NVIDIA's EGL beside its Vulkan
driver. A driver's name in FNL_GPU_DRIVER binds it before the first
frame; anything else (`switch`) binds none, the GPU moved only to load
NVIDIA's module and back (`app_gpu_drivers_start()`) or by a ranking that
opens on another driver. Off by default,
because the entries are moves a walk over the list would make.

**Measured on schenker, KMS** (`tools/probe/drivermove.c`, which asks for
the entry itself, release tree): four moves, nvidia to nouveau and back
twice, the window drawn on the new driver each time and its titlebar in
the scanned-out frame after each -- 1.32 and 1.37 s to nouveau, 2.10 and
2.14 s back, each the helper's 0.85 and 1.9 s and the rest the renderer
built again; nothing in the kernel's log. On aspire (GTX 960M, the same
software, its stick's NVIDIA entry) the same, 1.70 to 1.81 s to nouveau
and 2.08 to 2.96 s back, the helper's part 1.5 and 1.9 s. Two things
stood in the way, and both cost every program, not only a move:

* *NVIDIA's userspace with its module unloaded.* Its Vulkan driver, and
  the implicit layer that loads it into every instance, try to load the
  module whenever they are loaded (`nvidia-modprobe`, refused while
  nouveau holds the GPU: "already bound to nouveau" in the kernel's log)
  and take about 2.5 s over it, with the load blocked or not. The first
  move took 30 s. With the module loaded and nothing bound to it they
  cost 30 ms an instance (two instances 352 ms on nouveau, 291 without
  NVIDIA's files), which keeping it loaded gives for nothing. Hiding the
  files instead -- copies of their directories mounted over them while
  nouveau is bound -- was tried and left: with the module kept loaded
  there is nothing to hide, and the mounts outlived a helper that died
  between making and taking them down.

  *Booted on nouveau, measured* (2026-10-25, both laptops on the keys'
  NVK entry, release). The module will not load at all while nouveau
  holds the GPU -- `modprobe nvidia` says "No such device", the kernel
  "No NVIDIA devices probed" -- so nothing keeps it loaded, and with
  NVIDIA's files in place each Vulkan instance and EGL display tries
  again: nine tries of 0.85 s before the first frame, which came at 13.6
  s on aspire and 19.5 s on schenker, where the same start with the
  module loaded first drew at 3.6 s. With FNL_GPU_DRIVER on, from
  nouveau with the module not loaded, `app_gpu_drivers_start()` loads it
  by a move there and straight back, before any renderer (1.5 and 0.7 s
  on aspire): the first frame at 5.0 s, no try refused, and the first
  move onto NVIDIA's module 0.79 s where it was 2.28, a bind and not a
  load. The keys' NVK entry puts NVIDIA's Vulkan driver, layer and EGL
  vendor aside at boot (`fenestal-nvidia-userspace`), and there the GPU
  moved to NVIDIA's module is listed by no Vulkan driver: that landing
  misses, is undone, and that renderer's move there is offered no more
  (`app_gpu_driver_unseen()`), since every later one would miss the same
  way. A round on a kernel driver there puts the files back for itself,
  and every other round puts the GPU on the boot's driver and the files
  aside again (`Bare.as_booted()`), whatever the round before it left.

  NVIDIA's module makes none of its nodes, and `nvidia-modprobe` takes
  one branch a call -- with `-u`, `-c 0` names a minor of UVM's -- so the
  helper makes `/dev/nvidia0` and `nvidiactl` (`-c 0 -c 255`), UVM's and
  modeset's a call each. On NVIDIA's entry its userspace has made them by
  then; booted on nouveau nothing had, and every move onto the module
  waited out `ready()` and went back.
* *The instance extensions asked once each.* Each asking opens every
  driver's library, and `make_instance()` asked for each optional
  extension apart -- about a dozen loads an instance. It asks once now
  (`instance_exts()`), which took the move from 30 s to 10.

With both, and the drivers letting go each their own way, four moves in
a window each way (2026-10-23, release): schenker 1.14 to 1.35 s to
nouveau and 0.77 to 0.84 s back; aspire 1.58 and 0.73 s. The rest of a
move to nouveau is the renderer built on NVK. Forced in the gc tree on
schenker, `gpu-driver-missed` -- the build taken as landing elsewhere --
put the GPU back on NVIDIA's module in 0.57 s and the window on Intel's
GPU, drawing; `gpu-driver-refused` left the GPU where it was and the
window on Intel's.

The harness's rounds on another driver are KMS rounds
(`kms-vulkan-bare@schenker:nouveau`, a round per driver bare-hosts names
after the machine): every server stopped, the GPU moved, then the round
as ever, drawn on the NVIDIA GPU by FNL_GPU. Run on schenker
(2026-10-23, release): `:nouveau` 59 checks and `:nvidia` 62, none failed,
each rebinding first and drawing on that driver, the GPU box's walk over
every entry -- Vulkan's and OpenGL's -- and back included, and, the move
offered (`FNL_GPU_DRIVER=switch`, which a round on a driver sends), the
box's last row taken by the round's hands, there and back
(`gpu_driver_moves()`: the kernel's time, drawn across or the pause said,
a move and not a loss, the size kept; on schenker 0.51 s onto NVIDIA's
module and 0.12 back, on aspire 0.42 and 0.74, every one drawn across);
run again with
each driver letting go its own way, the same checks passed and the
rebinds took 0.53 and 0.58 s, where they took 0.82 and 1.89. Run once
more after the review of the move (2026-10-23, release, both laptops
rebooted first), on aspire as well: the same counts on each, none failed,
the rebind onto nouveau 0.13 s on schenker and 0.73 on aspire, onto
NVIDIA's module 0.48 and 0.45; and four moves each way from OpenGL after
them, 0.79 to 0.84 s a move on schenker and 0.96 to 1.41 on aspire,
every one drawn across.

### Drawn across the move

A move to another kernel driver with nothing drawn meanwhile freezes the
window for 0.8 to 1.35 s on schenker, the helper's half and then the
renderer built on the GPU moved.
The panel is never the GPU's that moves -- the KMS backend holds the card
it scans out on, and lists no move where that is an NVIDIA GPU's -- so the
window can go on drawing on another GPU while the move runs, Intel's on
the laptops:
`renderer_swap()` builds Vulkan there first, whatever renderer the move
lands on, starts the move on a thread of its own (`mover`), and draws;
the render loop looks for its end each turn (`app_gpu_driver_poll()`) and
owes the swap onto the GPU moved, by its name, the move already made. The
caller sees two moves, each bracketed by `on_gpu_lost` and
`on_gpu_created`, and a request for another GPU in between waits for the
second. The window is closed only once a move under way has ended.

What draws across must hold nothing of the GPU, and an ordinary Vulkan
instance holds it: the loader loads every driver installed, and NVK and
NVIDIA's open the GPU at the instance (the probe around
`vkCreateInstance`, above). So an instance made across a move is made of
Mesa's drivers for Intel's and AMD's GPUs and its CPU rasteriser alone,
each of which opens no GPU of another vendor's -- never NVK, which opens
the GPU moved -- opened by the library and handed to the loader in its
exclusive mode,
`VK_LUNARG_direct_driver_loading`, their instance extensions asked of each
driver directly, since the loader's own asking loads every driver too.
Every instance made until the move ends is one of these
(`App::gpu_cross`), a rebuild after a loss included; and their GPUs are
neither listed nor published, so the titlebar's list stays as it was.
The loader also loads the implicit layers, NVIDIA's among them, which
nothing per instance turns off; so the renderer built across is then
asked what it holds (`FnlBackendVtbl::gpu_driver_held`: a descriptor on
the GPU's own nodes, and on NVIDIA's module any `/dev/nvidia*`), and one
that holds anything is torn down, the move made as before with the frames
paused, and every later move with them. So is a loader without the
extension, a machine with none of those drivers, or a thread that will
not start: the pause is the fallback, said once, and nothing worse.
`FNL_FAULT=gpu-driver-uncrossed` forces it.

**Measured on schenker, KMS** (2026-10-23, release, `drivermove`, which
counts the frames drawn while a move is under way, and the moves asked of
the socket): the loader takes Mesa's drivers handed to it, and NVIDIA's
layer in that instance holds nothing of the GPU, so both moves were drawn
across. To nouveau, 0.84 s from the request to a frame on NVK, 13 frames
drawn meanwhile on Intel's GPU and 0.57 s the longest without one; back,
0.78 s, 34 frames and 0.15 s. The helper took 118 and 487 ms of those,
behind the frames, so what pauses is the renderer built on the GPU moved,
NVK's the slower; the swap onto Intel's is within the 0.15 s. With
nouveau not loaded ahead its bind took 495 ms, and loaded ahead 73. On
aspire (GTX 960M), two runs of a move each way: to nouveau 1.86 and
1.87 s, 78 and 50 frames, 0.30 and 0.78 s the longest without one; back
0.96 and 0.99 s, 33 frames each, 0.30 and 0.31 s. nouveau's bind there is
slow loaded ahead too, 1152 ms not and 715 ms loaded -- the GPU's own
start in nouveau, which logs the PRIVRING read faults it logs at every
bind on that GPU -- and all of it is behind the frames.

Those faults, `bus: MMIO read of 00000000 FAULT at 619444 [ PRIVRING ]`
three at a time at every resume of the GPU under nouveau and one at
`6013d4` at every bind, 76 in an hour of rounds, are the graphics
engine's init running a VBIOS script that reads a display register of a
GPU with no display (`gm107_gr_init_bios_2`; freedesktop bug 111392 on
the same GM107 laptops, harmless, no fix), never while the GPU idles, and
none of them a probe of ours: nouveau's card there has no connector to
probe. No knob skips the script -- `config=disp=0` was tried, and the
reader is the graphics engine's -- so `bare_driver.py --setup` quiets
nouveau's bus subdev on a machine with an NVIDIA GPU
(`/etc/modprobe.d/fenestal-nouveau.conf`, `debug=bus=fatal`), every line
of whose it has to say there is this one; measured on aspire, the bind
that always logged one logged none. Read when the module is loaded from
the root filesystem, which the helper's loads are; the keys' nouveau
entries load nouveau from the initramfs, so they carry the same option on
their command line (in the boot images' repository; put on the two keys
here by hand, in the ESP's entries and `/etc/fenestal/boot/options-*`, the
menu written again), and aspire booted on its nouveau entry logged no
fault.

**Where the pause goes** (2026-10-23, schenker, release; each swap says
what its build's three steps took, `rebuilt on … in 529 ms (instance 180,
surface 181, objects 169)`). The landing on NVK takes 530 to 560 ms: the
instance 180 to 200 ms, which is the loader's, every driver loaded; the
surface 180 to 190, which is NVK's first enumeration of its devices,
inside `vkGetDrmDisplayEXT`; the device 130 to 135; the pipelines 20 to
22; the swapchain 10 to 13. On NVIDIA's driver 102 to 110 ms (56 to 64,
4, 40 to 43), and the renderer drawn across, on Intel's GPU, 35 to 50 ms.
On aspire NVK's is 244 ms (179, 41, 24). Zink's open on NVK, a move of
OpenGL's, is 730 to 760 ms, all but 30 of it its display's. Every part of
it is a driver's; the pipelines are no part of it, since Mesa's and
NVIDIA's own disk caches cover the compiles, and the library's cache
file, one a device (*The startup inventory, and `--gpu=<n>`*), saves the
rewrite each move cost. A move that misses pauses longer, 1.27 s on
schenker from OpenGL: the landing's build, the undo and the build back,
with nothing drawn across, since the landing is where the cross ends.

**Killed halfway, on schenker** (2026-10-23, release). The program
SIGKILLed 6 ms into a move to nouveau: the service made the move all the
same, 0.6 s, and ended well, the GPU on nouveau with its nodes; its one
complaint, in the journal, the status line it could not write. The
service frozen by its cgroup the moment the GPU had no driver -- inside
`rmmod nouveau`, on a move back -- and SIGKILLed: systemd failed it and
started the rescue, which bound NVIDIA's module 0.57 s after the kill;
the program, its move ended with no answer, found the GPU on none, undid
the move once the rescue let go of the lock -- NVIDIA's to nouveau, 0.5 s
-- and drew on Intel's GPU again, where it had been.

### Ranked by measured frame rate

Which entry of the GPU list draws a program fastest is not a property any driver
reports. It is the program's scene's and the machine's: on schenker the GTX 1050
Ti under NVIDIA's driver against the same GPU under NVK at its boot clocks, and
OpenGL against Vulkan on each. The kind a driver gives (discrete, integrated)
orders GPUs and says nothing of the rest; and a display's size and rate are part
of what a frame costs. So the list's order is measured (`src/core/rank.c`),
entry by entry and display by display, and only what is not known: once the list
is whole -- every renderer listed since the last move to another kernel driver,
nothing moving -- and again whenever the window is on another display or the
list has an entry it had not, the entries with no figure kept for the display
the window is on. A display new to the program measures every entry on it, the
first time the window is there -- the window is never moved to another display
to be measured; a GPU new to it measures that GPU's entries. A GPU or a display
taken away keeps its figures, so nothing is measured when it comes back; an
entry that would not draw is kept at 0, or it would be tried at every start. The
window taken to another display while it is measured -- its own unplugged, which
on KMS moves it (*Monitors come and go*) -- ends the calibration with what was
measured where it was, the display it is on is asked about afresh, and the
window goes to that display's first entry, not left on the one it was measuring.
Until a display has figures the list is ordered by each entry's best on any
other. `FNL_GPU_RANK=calibrate` (the testbed's `--gpu-rank=calibrate`) measures
every entry on this display again once, `keep` never measures. The calibration
visits the entries by the request `fnl_request_gpu()` makes, the entries on the
driver the GPU is on first and then the moves, so the GPU moves once and back
once; within each, the likeliest to win first -- hardware before software, then
by what each is known to do here or elsewhere (`order_plan()`). On each it draws
with the scene moving (F2's clock, run by `App::calibrating` beside the caller's
own setting and never stored into it, so that an F2 or a
`fnl_set_scene_motion()` made meanwhile is not undone at the end), so a switch
is measured as a frame in flight would meet it, until the figure holds still:
watched in quarter-second spans, it is settled once two in a row agree within 5
% (the clocks have risen and the means caught up), after 0.5 s and by 1.5 s
whatever the spans say; and then sampled until the running mean moves less than
3 % in a span, after 0.5 s and by 2 s. One settled under half of the best
measured on the display is kept at its steady span's figure and not sampled: it
decides nothing at the top of the list, and the figure is low at worst for an
entry that loses either way.

Every entry took the whole 1.5 and 2 s before (2026-11-01), which on this
desktop was 11 s for `texture`'s three entries. Logged a figure a frame
over 4 s from landing, the RX 590's two entries were flat from 0.5 s, a
second's settle and sample within a few percent of the 3.5 s figure, which
itself spread 7 to 14 % between runs; llvmpipe climbed from 88 to some
430 fps over two seconds, a sixth of the slower hardware entry throughout.
Calibrated again, three entries took 4 to 5 s and two took 3, on X11 and
on Wayland: the hardware Vulkan entry in 1.0 to 1.1 s at 2855 to 3244 fps
against the old 3061 and 3087, OpenGL in 1.7 to 2.0 s at 1847 to 2152
against 2021 and 2152, llvmpipe in 1.4 to 2.4 s, and the same winner every
time. On schenker's KMS (2026-11-01, the `texture` demo,
`FNL_GPU_DRIVER=switch`, both codes from NVIDIA's own driver) eight
entries took 13 s where they took 32,
ranked in the same order with figures within 1 to 4 %: the HD 630's Vulkan
first at 879 against 875 fps, the GTX 1050 Ti's Vulkan on NVIDIA's driver
second at 395 against 409, settled in 1.7 s -- NVIDIA's clocks rising
took longer than the minimum and far less than the 1.5 s cap -- and six of
the eight, all under half of the HD 630, kept at their settled figures,
the two moves to nouveau included (126 and 127 against 127 and 127).
The one figure that moved is the last entry's, OpenGL on llvmpipe, 80
against 58.

Meanwhile the window says so, over everything it draws, the titlebar
included: a panel with the time left -- each entry still to measure at
what those of its group measured took, the entries on the driver the GPU
is on and those after a move to another, or before one has been at 1.7 s
(the least settle and sample, a span more of each, and a switch) and 1.8 s
more for a move (schenker, 2026-10-26, when every entry took the whole
settle and sample: 3.7 s an entry, 5.7 and 4.7 s after the move to
nouveau) -- a bar, and the entry being measured
(`app_rank_draw()`, after the titlebar's drop-down in `draw_overlay()`),
one width for the whole calibration -- the widest line any of its
entries will show, and at least 640 points -- since a panel sized to the
line on screen changed width with every entry's name.
And it takes no input (`app_rank_holds_input()`, read in core's input
entry points for every backend): a press would move it off the entry
measured, a key could close it. Presses, motion, scroll, characters and
repeats are dropped and no binding acts; a release goes only for a key or
a button whose press went, so one held before the calibration began is not
left held and none pressed during it reaches the caller as a release
alone. A window server's own close still closes it.

The figure is the frame's cost, 1000 / the larger of the GPU's
milliseconds (the renderers' timestamps) and the CPU's (the render
thread's CPU time, *What a frame costs, compared*), which the library
keeps at every frame for `fnl_frame_gpu_ms()` anyway.
Counting presents was the alternative: it needs vsync off, which tears for
the length of the calibration, and on KMS's copy path it counts the copy
as much as the GPU. The cost needs neither and is the program's own scene
at the window's size; it reads a GPU kept at low clocks by a light scene
as slower than it could be, which for that scene it is.

The figures are kept per program and backend, in the user's cache
(`$XDG_CACHE_HOME/fenestal/gpu-rank-<program>-<backend>.txt`, under
`~/Library/Caches` on macOS and `%LOCALAPPDATA%` on Windows), one line a
figure, `<fps> <display> <entry>`, read at every start, and written
beside itself under a name with the process's id and renamed over: two
instances of a program share the file. A display is
the output the window is on as the backend names it and the mode it
shows, `HDMI-A-1 3840x2160@30` -- KMS's mode, or a window server's output
size and rate as published: a frame's cost is its size's (the copy path
at 4K is four times 1080p's), and a monitor swapped on a cable is the
same connector at another mode, which the connector alone would have
given the old monitor's figures. So another mode, picked from the
titlebar or a monitor's own, is measured once, and a monitor swapped for
one at the same mode keeps the figures, which still hold.

On a window server the display is named the same way, from what the
backend publishes for its status line cut to the output it follows: the
model in brackets left off, and on Wayland the other outputs a window
straddles (`eDP-1 (LG Display 0x046D) + HDMI-A-1 (...)`), or every
crossing between two monitors was a calibration of a display nobody has.
The rate is RandR's on X11, the mode's dot clock over its total, and
`wl_output`'s current mode on Wayland, which had none published and
keyed every display `@0`. X11 reads its rates a pump turn after the
window opens, so at the start, where the GPU is picked, a rate not yet
said takes a kept display of that output and size; and figures kept @0,
by a first start that knew no rate, become the rate's once it is known
and none are kept for it, the file rewritten so -- or the output is
measured again under a second name. Win32 under wine is the other
backend that reads late: wine's DISPLAY1 was kept as `@0` and `@60` side
by side on this desktop, three entries measured twice (2026-10-31).
Wayland names no output before the window is shown: the start orders by
each entry's best anywhere, nothing is measured until the window enters
an output, and it then goes to that display's first entry. The window,
not the display, is
what is drawn there, so a figure is the window's size's on that display
-- 1280x720 on both of schenker's below.

Driven on schenker on 2026-10-26, nothing named, on its bare Xorg (no
window manager) and bare weston: the first start measured every entry on
the panel, `eDP-1 1920x1080@60` -- four under Xorg, where OpenGL lists
the server's GPU alone, five under weston, which has NVIDIA's OpenGL too
(*OpenGL lists what its drivers will open this window on*) -- in 15 and
18 s, and opened on the first, NVIDIA's Vulkan (3621 and 3200 fps). The
window dragged to the 4K HDMI monitor measured every entry there, the
NVIDIA entries read lower across to the Intel-driven output (Vulkan's
967 and 883 fps); under weston NVIDIA's OpenGL came first there (1560)
and the window moved to it, and back on the panel to NVIDIA's Vulkan,
nothing measured either way; a window straddling the two measured
nothing. A second start on each opened on the first with nothing
measured. The GPU box listed the ranked entries with their figures and
the Recalibrate row. The GPU box
offers the calibration by hand too, an unnumbered row after the GPUs
("Recalibrate"), so a row is still its place in the list: every entry
on this display measured again now, and the figures of every other
display connected dropped, to be measured when the window is there; a
display not connected keeps its own (`app_rank_recalibrate()`). Not
where the ranking is kept or ordered alone (`keep`, `order`), as a
harness round's is, which would otherwise click it as a GPU: under `keep`
on schenker's KMS the box lists its six GPUs, ranked, and nothing after
them. Driven there by hand: eDP-1 measured once under
`eDP-1 1920x1080@60`, HDMI's 3840x2160@30 and 1920x1080@60 once each;
Recalibrate dropping the 24 figures of the connected displays, measuring
HDMI's again and eDP-1's at its next visit. An entry is keyed by its
renderer and its name cut to the GPU's (`app_gpu_short()`) -- a
software rasteriser by its kind alone, since OpenGL lists Mesa's as "Mesa
software rasteriser" and names it "llvmpipe" once it draws -- and for the
GPU that can move to another kernel driver by the name it keeps on either
driver and the driver -- `gl|NVIDIA GeForce GTX 1050 Ti on nvidia` -- so
a key holds across the moves, NVK's and Zink's names and NVIDIA's
"/PCIe/SSE2". Without FNL_GPU_DRIVER the list names no driver, and that
GPU's rows go by their own names: a ranking measured with the moves
offered matches them only where the names differ by driver.

Driven on this desktop's GNOME, mutter placing the window (2026-10-28,
the RX 590, DP-1 and DVI-D-1): on Wayland a calibration of three entries
on DVI-D-1 in 11 s; on X11 three on DP-1 in 11 s at the start, a second
start measuring nothing and opening on the first entry, and the window
dragged onto DVI-D-1 measuring that display's three in 11 s, at its
rate (`DVI-D-1 1920x1200@60`). The start's line names the output X11
says before mutter places the window, which may not be the one it lands
on (`the window on DVI-D-1 1920x1200@0`, then shown on DP-1); the
ranking follows the window once it is shown, as it does a move. The
size in a window server's display is the output's area as published,
the panel and dock left out (`DP-1 3840x2134` on X11), so the dock moved
or hidden is a new display, measured once -- but on Wayland it is the
followed output's own mode (`DP-1 3840x2160`), published with its name
(`app_publish_output_mode()`). Wayland's area is
`xdg_toplevel.configure_bounds`, which the compositor sends for wherever
it reckons the window is, and that is another output's while the window
straddles two: keyed by it, this desktop kept four displays for its two
monitors (2026-11-01) -- `DP-1 3773x2134`, `DP-1 3840x2160` (the mode,
standing in after a move until bounds came), `DP-1 1920x1200` (DP-1's
name, DVI-D-1's bounds) and `DVI-D-1 1920x1200` -- and a window resized
across onto DVI-D-1 calibrated twice, once straddling and once there
whole. X11 takes the name and the area from the one monitor and never
mixed them. A Wayland figure kept under its area before this is under a
name no window has now, and its display is measured once more.

**A display is acted on once the window has stayed on it.** The list is
ordered for the display the window is on at once, but what that sets
off -- a calibration of what is missing there, and the move to its first
entry -- waits until the window has been on that one output, at one
size and straddling no other, for a second (`RANK_QUIET`, `quiet()` in
`rank.c`). A window carried or stretched across monitors would otherwise
measure the output it is passing over, or move to another GPU in the
middle of the drag. A recalibration asked for from the GPU box does not
wait.

And on the other two laptops' sticks, the same day, nothing named but
the backend, a calibration and then a second start on each (every second
start measured nothing and opened on the first): aspire, on eDP-1 at
1920x1080, six entries on KMS in 22 s -- the HD 530's Vulkan first by a
hair (397 against the GTX 960M's 394), the 960M's OpenGL copying since
NVIDIA's EGL takes no Intel buffer -- four under Xorg in 15 s and five
under weston in 18 s, the 960M's Vulkan first on both (1950, 1625);
thinkpad, on LVDS-1 at 1366x768, two on KMS in 7 s, llvmpipe's OpenGL
first (61 against lavapipe's 47) now that it draws onto the output's
buffers, and lavapipe alone under Xorg and weston, where OpenGL offers
Mesa's software rasteriser only to `LIBGL_ALWAYS_SOFTWARE=1` and the
Ironlake GPU has no 4.5.

With a ranking the list is sorted by the figures for the window's display
in `gpu_merge()`, the measured entries first and the rest in the list's
own order after them, and each row of the GPU box says its figure; a move
to another display sorts it again, and puts the window on that display's
first entry, once anything missing there is measured -- unless the
ranking is only kept (`keep`), as a harness round's is, whose moves
between displays are checked as moves of the display alone. The keys
hold across the moves, so the
order does too, and every entry keeps its number for the window's life as
before. A window that names no GPU opens on the first entry it can make
(`open_on()`): its renderer tried first, its GPU asked for by the name its
key holds -- which `app_name_has()` matches with the vendors' "(R)" and
"(TM)" in the driver's name or left out, as the key leaves them -- and its
kernel driver bound before any renderer is built, where it names one the
GPU is not on. A key it can make (`makeable()`) names a kernel driver only
with a move offered, and one of the GPU's two, and is then not that GPU's
key with none, kept by a run without the moves: read the other way,
either names a GPU no renderer lists, and the window opens where the
window server's pick puts it.
Where the pick went elsewhere all the same -- a software rasteriser's key
names no GPU -- the window moves to the first entry once the list is
whole, by the request a click makes. An index (`gpu_index`, a number in
`FNL_GPU`) counts the ranked list the same way; a name is matched as
before. Both stand for the window's life (`App::rank_asked_name`,
`rank_asked_index`, kept apart from `gpu_want` and `gpu_index`, which
every move spends): a calibration's end, or the window on another
display, aims it again (`aim()`) -- at the first entry where nothing was
asked for, at the index's entry of the ranked list, and for a name back
to the entry it was on when the calibration began (`App::rank_land`, by
its key), never at the first, which the calibration's own moves would
otherwise have made the window's. A renderer named (`FNL_RENDERER`,
`FnlInitDesc::renderer`) narrows both to its own entries
(`App::renderer_named`): the first entry of the whole list may be another
renderer's, and a move there would draw with one nobody asked for, which
the name promises never happens. The pick's
inventory lines are held until the list is merged and
said with its numbers (`app_gpu_inventory()`), and once the other
renderers have listed theirs the whole list is said again, the moves
included (`GPU 7: <GPU>, on nouveau (discrete, Vulkan)`): a part joining
may move the entries said before it, which the numbers a harness clicks by
must follow. The harness sets `FNL_GPU_RANK=off` for every program it
starts: a round names the GPU it tests -- but for a round on a kernel
driver, which sends `keep` where the machine keeps a ranking and opens on
its first entry, and otherwise `FNL_GPU_RANK=order`: the list ordered and
nothing moved, the window where the round's FNL_GPU puts it, so that the
round walks the list as it is used (`gpu_every_pair()` in `stress.py`:
from every row to every other, each direction once). A ranked round
names no renderer, so the tree's two KMS environments would run the
same round under two names: `kms-vulkan-bare` runs it, and
`kms-gl-bare` is skipped there as its twin (`ranked_twin()`). Naming
the renderer stays what tests each renderer's start where no ranking is
used -- a machine that keeps none, and ThreadSanitizer, which NVIDIA's
own driver crashes -- the Vulkan layer's check with it.

Under Windows, where no driver moves (2026-10-31, `1d987ac`,
`FNL_GPU_RANK=calibrate`): five entries in 21 and 22 s, each Vulkan entry
measured on its GPU's default way, through DXGI. schenker: Vulkan on the
GTX 1050 Ti 1,486 fps, Direct3D 12 on it 1,255, Intel's Vulkan 755,
Intel's Direct3D 12 725, WARP 99; aspire: Vulkan on the GTX 960M 1,641,
Direct3D 12 on it 1,408, Intel's Direct3D 12 862, Intel's Vulkan 609,
WARP 123. Both open on Vulkan on the NVIDIA GPU, the window moved back
there when the calibration ends, every move a swap of renderer where the
renderer changed. A file ranked before the way was folded into the GPU's
key holds rows of the old key in either case, `-d3d11|` and `-D3D11|` --
schenker's had the second, which the reader then kept and printed as
ranks 2 and 4 -- so the reader drops both spellings.

**A GPU plugged in or out** -- a dock's, an external one -- is heard on
KMS from the same uevents as a monitor (*Monitors come and go*): a DRM
card node added or removed, of any card, but for the GPU a move to
another kernel driver takes away and brings back, whose card goes and
comes with each move and whose landing lists again itself
(`app_gpu_driver_owns()`). The list is then made again from every
renderer at the render loop's next quiet turn (`app_gpu_hotplug_poll()`),
1.5 s after the render loop first sees it, since the kernel's uevent
comes before udev has given the new card's nodes their group -- the wait
begun again by each card heard during it, and one heard after it owed a
list of its own (`App::gpu_hotplug_gen`, a count and not a flag, which a
second card heard in the wait would have been spent with unwaited): by a
rebuild on the GPU in use, by its name, that lists the other renderers
first, as a landing on another kernel driver does -- a listing after the
first frame would put a Vulkan instance beside NVIDIA's live EGL
display, which ends it. A GPU new to the list then has its entries
measured as any entry with no figure is, and one that went keeps its
figures; a GPU in use that went is a lost device first, answered on
another. **Untested**: no GPU here can be plugged, and nothing has
driven it; the window servers' backends hear no GPU come or go yet.

### What a frame costs, compared

The ranking puts every renderer's entries in one list by
`fnl_frame_gpu_ms()` and `fnl_frame_cpu_ms()`, and each renderer timed
both over a span of its own: Vulkan's GPU span held the copy path's
readback and OpenGL's did not; Vulkan's CPU span held the submit and
OpenGL's did not, nor the swap, nor the readback OpenGL's copy path does
in its present. `docs/comparable-frame-costs.md` is the plan that makes
the spans one, in phases; this is what was measured before the first and
what each phase has changed.

**The measurement.** `FNL_COST_PARTS=1` beside `--frame-times`
(`src/core/parts.c`, every tree) adds lines after each frame line: the
render thread's CPU time over the spans the line gives in wall time, the
GPU span cut by four bottom-of-pipe stamps on Vulkan, and on Linux each
thread's CPU a frame. The testbed, its scene moving, vsync on, on four
places (milliseconds a frame; the plan has every row and figure):

| place | Vulkan GPU | of it, the readback | OpenGL GPU | OpenGL's present, CPU | drawn at, Vulkan / OpenGL |
|---|---|---|---|---|---|
| schenker KMS, GTX 1050 Ti, NVIDIA 580 | 1.79 | 1.28 | 0.62 | 8.0 | 60 / 60 |
| schenker KMS, NVK | 8.79 | 5.85 | -- | -- | 60 / -- |
| thinkpad KMS, lavapipe / llvmpipe | 19.7 | 1.8 | 17.8 | 3.4 | 30 / 30 |
| this desktop KMS 4K, RX 590 | 0.56 | none (its own output) | 0.31 | 16.3 | 60 / 30 |
| this desktop KMS 4K, lavapipe / llvmpipe | 42.3 | 6.0 | 23.4 | 20.6 | 18 / 20 |
| this desktop Wayland 1280x720, RX 590 | 0.16 | none | 0.20 | 0.21 | 60 / 60 |

Direct3D 12's start stamp is the list's first `EndQuery`, and it is
written once the work ahead of it is done (`tools/probe/d3d12ts.c`,
2026-10-31): a list of large copies, the next list's start stamp, both
put on the CPU's clock by the queue's calibration beside a fence the CPU
waited on -- the start after the copies' end in every trial, 80 a GPU,
submitted apart or in one call: schenker's HD 630 by 0.006 to 0.019 ms
and GTX 1050 Ti by 0.000 to 0.015, aspire's HD 530 by 0.005 to 0.024 and
GTX 960M by 0.002 to 0.037, and vkd3d-proton under wine on this desktop's
RX 590 by 0.007 to 0.02. So D3D12's span holds no tail of the frame
before, as Vulkan's does not.

What did not differ, anywhere: a top-of-pipe stamp against a
bottom-of-pipe one at the span's start (0.002 to 0.06), a wait for the
window's image inside the span (none: the copy path has no image, RADV
and ANV showed none on their own outputs), and the end of the render
pass with its change of layout (at most 1%). The driver's threads did
little -- radeonsi's submission thread 0.11 to 0.14, NVIDIA's Vulkan
0.15 -- except a software device's rasterisers, which are its GPU and
as busy as its span is long.

What did: **the copy path's readback, inside Vulkan's GPU span and
outside OpenGL's**, a third to two thirds of Vulkan's on a hardware
device's copy path; and **the CPU the render
thread spends outside the span it is timed over**: OpenGL's swap on
radeonsi (0.21, which is the whole of OpenGL's lead in this desktop's
Wayland ranking), and on KMS every copy path's present -- reading the
frame back and copying it into the scanout buffer, 16.3 ms of the RX
590's OpenGL frame at 4K, which is why it draws at 30 where Vulkan draws
at 60, and the ranking still put it first. And the other way: llvmpipe
blocks the render thread inside OpenGL's recording, 22 ms of wall clock
with 1.3 of CPU, which the wall-clock CPU half counted as work.

**The GPU half: the frame drawn, the copy path's readback in it.** The
copy path's readback is GPU work every frame on that path costs, and
the frame rate there is bound by it: drawn flat out on schenker's
console, Vulkan on the GTX 1050 Ti presented at 390 a second against the
1634 its figures allowed with the readback (1.29 ms) left out, and
OpenGL on the Intel GPU at 102 against 333 -- its `glReadPixels` into
client memory blocked the render thread 6.7 ms a frame on the GPU's
copy, a wait neither half counts. So Vulkan's end stamp follows the copy
path's readback, and OpenGL's copy path reads back through a pack buffer
(`pack_start()`) ahead of its end stamp, the present mapping it. A
snapshot's copy, once a move, is no part of drawing the frame: Vulkan's
and D3D12's come after the end stamp, and OpenGL and Metal, which cannot
stamp around theirs, do not sample a frame that takes one
(`slot_untimed` on Metal).

**The CPU half: the render thread's CPU time, to the present's return.** Every
renderer marks the end of its waits and the return of the call that hands the
frame over (`app_frame_mark()`), and the figure is the thread's CPU time between
them: the submit, OpenGL's swap and the copy path's readback and copy are in it,
and a wait that blocks -- FIFO's in the present, llvmpipe's inside the recording
-- is not, wherever it falls. The present can then be inside the span although
it is also the throttle, and the four renderers' spans end at the same point.
The clock is `CLOCK_THREAD_CPUTIME_ID` on Linux and macOS, a system call of
about a microsecond, two a frame; on Windows `QueryThreadCycleTime()`, the
time-stamp counter while the thread runs, turned into seconds at the counter's
rate measured against `QueryPerformanceCounter()` from the clock's first reading
(`GetThreadTimes()` counts 15.6 ms ticks). Wine 10.0's `QueryThreadCycleTime()`
fails (native Windows on aspire: a 200 ms busy loop reads 193.7 ms); there, and
wherever the time is not known, the figure falls back to a wall-clock mean over
recording and submitting (`App::wall_cost`). `late_cost` keeps that span for the
pacing, which needs wall time. Both means start again on another device, whose
CPU figure can differ by the whole copy.

A driver that waits by spinning would put its wait into the figure.
NVIDIA's OpenGL was the one suspected (`__GL_YIELD`), and measured on
schenker (NVIDIA 580.196.07, 2026-10-28) it does not, wherever it can
draw for this library. On X11 it cannot draw at all: its EGL lists
`EGL_EXT_platform_xcb` and answers every xcb display with
`EGL_NO_DISPLAY` and no error -- on the harness's Xorg, and on an Xorg
NVIDIA's own driver drove, the panel lit through PRIME -- where it takes
the same server through Xlib, which this library does not link; so
OpenGL there is Mesa's. Under PRIME offload on the Intel GPU's Xorg the
same: Xlib's display goes to NVIDIA, xcb's to Mesa. The report, with a
C reproduction, is drafted in `docs/nvidia-egl-xcb.md`. On weston its swap
does not wait for the refresh -- the frame callback paces, and the
interval is 0 -- and the present costs 0.37 to 0.50 ms, all of it CPU. On
KMS's copy path the present costs 6.6 ms of CPU in 8.3, and with
`__GL_YIELD=USLEEP` 7.3: work, not a wait spun; nor uncached reads, a
pack buffer asked for in client memory (`GL_CLIENT_STORAGE_BIT`) giving
6.6 again. Which of the map and the copy it is was not split. The pack
buffer is asked for there all the same (`pack_start()`): without the hint
NVIDIA's GL made it in video memory and moved it at the first map, and its
debug output said so, which failed every checker round's clean log that
moved the window onto NVIDIA's OpenGL on KMS.

Checked against a rate drawn, flat out: the predicted rate over the
rate presented is 1.35 to 1.46 on every run but OpenGL on the GTX 1050
Ti, which is CPU-bound alone and reads 1.01; before, 1.44 to 10.2
(`docs/benchmarks.md`). And the order the figures give is the order
drawn, on both machines.

The ranking file is versioned (`RANK_VERSION`, `v2` on its first line):
a file of another version is read as nothing measured and every entry is
measured again, since the halves changed under every entry kept.

### The veil, and what the screen shows across a move

A move to another GPU, renderer or kernel driver tears the renderer down
and builds another on the same window, and between the last frame of the
one and the first of the other the screen shows whatever the window
server or the kernel has. Read off the code before anything was done
about it:

| move | backend | between the two frames | because |
|---|---|---|---|
| GPU or renderer | X11, Cocoa | the last frame, frozen | the server keeps the pixmap |
| GPU or renderer | Wayland, after the first move | a transparent hole | `present_drop()` destroyed the old subsurface before the new had a frame |
| GPU or renderer | Win32, after the first move | whatever GDI last painted under the window | the frames never touch the redirection surface, and nothing painted the fresh child window |
| GPU, off the display path | KMS | black | `kms_scanout_take()` set a zeroed dumb buffer before the driver removed its framebuffers |
| kernel driver, drawn across | KMS | frames on Intel's GPU, then black for the landing | the renderer drawn across took the display, so the landing paid the take-back and a mode set, over NVK's 530 to 560 ms build |
| mode | KMS | black, then the monitor's own resync | the mode set itself |
| resize, fullscreen | window servers | continuous | nothing to hide |

The nagging blank was therefore a KMS hand-over with a deliberate black
frame in it, not a rendering cost. Three things answer it, in layers.

**The last frame is held wherever that is free.** Across a move to
another kernel driver the renderer drawn meanwhile takes the copy path
whatever it could take (`create_surface()` in `surface_kms.c`): the
backend's own pair of dumb buffers then stays on the output from the last
frame before the move to the first on the GPU moved, which lands by the
copy path too, so no display changes hands for the landing, no output is
taken back, and the `SETCRTC` the copy path's first frame makes after the
renew is at the mode already set, which is a plane update and not a mode
set. The copy's cost, a few per cent of a core on the GPU with the panel,
is paid for the second the move takes. On Wayland the subsurface the
frames went into is kept under the one made at a renew until that one has
shown a frame -- the first callback asked of the new surface, answered
(`present_settle()`, `BackendState::present_old`) -- and at the first
renew the window's own surface keeps its last frame the same way, the
transparent pixel attached only then (`backing_owed`): a compositor keeps
on screen a buffer its client has destroyed, which GNOME was measured to
do when a window moved to lavapipe kept showing the last hardware frame
(*Moving to another GPU*), and weston as well (below). A second renew
within one move -- a surface or a device that would not build, and the
way back -- finds the subsurface made at the first still empty, so it
drops that one and keeps the one under it, which is still the last frame
on screen (`present_drop()`). OpenGL's recovery renews as the others'
do. X11 and Cocoa needed nothing.

**The frame is read back ahead of the move, and the new device dissolves
from it.** When a move is asked for, the next frame drawn reads itself
back (`App::snap_owed`, `app_snapshot_set()`): Vulkan records a copy of
the swapchain image into a host buffer after the window's pass, legally,
since the image is this frame's, acquired and not yet presented -- a
presented image is the presentation engine's until it is acquired again,
which is why the snapshot is not taken at the teardown -- and waits the
frame's fence once (`record_readback()`, with `TRANSFER_SRC` asked of
every swapchain whose surface allows it, which every one measured does;
the copy path's readback already has every frame); OpenGL reads the
window's image back as its surfaceless present does; Direct3D 12 copies
the back buffer into a readback buffer and waits the slot's fence; Metal
blits the drawable into a shared buffer and waits the command buffer,
which is why the layer is no longer framebuffer-only. The gate
(`app_veil_gate()`, `VEIL_SNAP`) lets the move go once the frame is in
hand, `VEIL_STALL` after asking at the latest. What the backend can then
show through the gap it shows from that frame: the KMS take-back buffer
is filled with it rather than zeroed (`scan_fill()`), and the Win32 child
a move makes paints it in its erase until its first present
(`present_erase()`, a stretched top-down DIB). That erase is painted on
the window's thread as the child is made, synchronously and before the
old child is destroyed, and the mark that ends it
(`BackendState::present_presented`) is cleared on that thread before the
child exists: a child's GDI paint lands in the window's redirection
surface, which the frames never touch, so the surface under the old child
holds the erase of the move *before*, and the desktop shows it from the
old swapchain's going until the new child's first paint. Left to the
queue, that paint came one to four composed frames after the old child
went (measured below, under Vulkan), and marked from the render thread
once the renew returned, it raced the mark and lost it altogether under
Direct3D 12: the window showed the frame of the move before for the whole
build, 300 to 750 ms. The frame read back serves the one move it was
read for: it is let go when the next move is asked for and when the lift
ends (`snapshot_drop()`), so a later gap with no frame of its own -- a
move whose readback failed or was not asked for, a KMS take-back at a
mode change or a loss -- shows black and not a frame from minutes
before. And it is owed at rest too: the gate marks the recording stale
as it asks, which the rest rule reads as a change, since a window that
shows nothing new draws no frame to read back -- the landing of a move
to another kernel driver comes from the mover's thread with nothing
changed, and would otherwise wait out `VEIL_STALL` into the black
fallback. The child's own surface is the other half of the same
problem: once a move's erase is in it, the desktop shows it whenever
that child's swapchain goes -- between the next move's teardown and its
renew, or through a loss's rebuild minutes later. So before any
swapchain on the frames' window goes, at a move, a loss or a swap of
renderer, the window's thread paints it with the gap's picture
(`FnlBackendVtbl::present_window_hold`, `present_hold()`): the frame read
back, which is the frame on screen, or black, which is the veil's, so
nothing is seen to change while the swapchain is up and nothing older
shows after. The renew and the hold are sent with a bound, and one the
render thread gave up on is not done when the window's thread comes to
it later -- a renew then would destroy the child the swapchain built
since is on -- so each is a ticket under `App::lock` (`ask_send()`),
taken by whichever thread comes first, and one the window's thread has
taken is waited for to the end, without a bound, since that thread is
then running it and not joining. The device built after the
move uploads it once, on first use (`App::snap_gen`), and draws it over
the whole window with the fetch's own shaders blended by one constant --
`VK_BLEND_FACTOR_CONSTANT_ALPHA` and its three namesakes, `glBlendColor`,
`OMSetBlendFactor`, `setBlendColor`, the alpha channel left as it is -- at
a weight that falls from 1 to 0 over `fnl_set_transition_ms()`'s length.
Direct3D 12's factor is the odd one: `D3D12_BLEND_BLEND_FACTOR` blends
each colour channel by the factor's own channel, and the constant-alpha
factor it has lately needs a feature check and an Agility SDK no Windows
10 ships, so the weight goes into all four channels there; with it in the
alpha alone, as the other three take it, the cover drew nothing on both
laptops and on WARP, and every move cut (measured below)
(`FnlRendererVtbl::overlay_cover`, drawn last in `draw_overlay()`, after
the glyphs, scissored to the window as the ring is not the window's). No
shader was added: the fetch's, with a fourth blend state. So the screen
shows the last frame, frozen, for as long as the build takes, and the new
device's frames come up through it; on a still scene the two match and
nothing is seen to happen but the titlebar's device name. The cover and
the buffer the frame was read back into are let go once the veil is down
(`FnlRendererVtbl::cover_release`, asked after each submit at
`VEIL_NONE`): kept, they were some 33 MB at 4K for the device's life.
Vulkan and Direct3D 12 drain first, once, as each drains to replace a
cover; OpenGL and Metal let go at once, a frame in flight keeping what it
reads.

**Black is the fallback, and the veil makes it intentional.** Where no
frame could be read back -- a surface with no `TRANSFER_SRC`, a device
that would not upload, a window that drew no frame to read -- the
transition is black's (`App::veil_black`): the window fades to black over
the same length ahead of the move, the gap is black whatever filled it,
and the first frames fade up from black, so the gap reads as the dark
between two halves of one fade rather than a window that went away. The
veil is `FnlDrawState::veil`, 0 to 1 in steps of 1/255, computed by
`veil_step()` in `src/core/frame.c` from a phase and a clock
(`App::veil_phase`, `veil_t0`) as `scene_time` is, so each step is one
re-record through the recording cache and the rest rule as they stand,
and a veil that stands costs nothing; the black is one capsule stroke
down the window's middle through the stroke pipeline, which blends
(`draw_veil()` in `draw.c`), so no renderer has a pipeline for it either.

The gate is one for both. `gpu_switch_owed()` lets a wish for another GPU
stand while the frame is read back, or the veil comes down, and spends
it then; the landing of a move drawn across goes through the same gate
from `app_gpu_driver_poll()`, so the frames drawn across are read back
before the swap onto the GPU moved, and the GPU moved dissolves from
them. `device_recover()` arms the lift (`VEIL_ARMED`) for a rebuild that
followed the gate, and for a loss during a lift where the gap showed the
cover whole (`App::snap_shown`: the KMS take-back, the Win32 hold), so
that the first frame after it does not jump back from the whole cover to
where the lift had reached; where the window server kept the last frame,
the lift goes on. A loss outside a transition gets none: it had no frame
read back, and a cut to black from a frozen frame would be worse than
the frozen frame.
The lift starts at the rebuilt device's first present rather than at
`on_gpu_created`, since the first frame on NVK is hundreds of
milliseconds away and the cover has to be up when it lands; nothing
else starts it -- `gpu_switch_owed()` runs every turn and lifts only for
a wish it spent, and `veil_lift()` leaves `VEIL_ARMED` alone. On Windows
the desktop shows that first frame 60 to 160 ms after it is presented --
a new child window's swapchain, which the compositor takes up late -- and
on the GTX 960M's Vulkan 220 ms, the second frame's acquire blocked
meanwhile, so the first third of the dissolve is not seen there, and on
that device most of it (measured below). Waiting for the first present to
be shown before the lift, through the present-wait and frame-statistics
signals the size handshake uses, was tried and changed nothing: both
answer before the compositor has the window, and a delay with no signal
behind it would be a constant of one desktop's. The lift stays at the
present. A wish that
arrives while a lift runs starts over, with the lift held where it
stands for the frame read back (`App::veil_snap`), cover and all, so
that frame is the one on screen and the next device dissolves from it
with nothing seen to jump. A wish that turns out to be for nowhere lifts
what came down (`veil_lift()`), from where it stood -- and a frame read
back with no lift under it is let go rather than dissolved over a scene
that has moved on. A window that is not on screen has no lift: nobody
sees it, and the first present that would start it never comes while the
window is hidden, so the veil is let go (`veil_unseen()`) and the frames
come back without it -- on a turn the backend or the renderer says
hidden, or on Wayland, where both say shown, once nothing has invited a
frame for `VEIL_STALL`, since a compositor may take a tenth of a second
over a new surface's first frame -- and on the turns a minimized window
on Win32 or Cocoa idles for having no size. A move of a window the
backend or the renderer knows to be hidden is made at once, whichever of
the readback, the fade or the wait for its frame it was in
(`veil_hidden()`), with no frame to read back or fade. A loss
while a frame is read back from a lift held, where the gap showed the
cover whole, reads it back at the whole cover, which is what was last on
screen.
A still window's move dissolves as a moving one's does: a kernel driver
move on KMS with the scene at rest (`tools/probe/dissolve.c --still`, the
scanout sampled by `screen_means.py`, 2026-10-31) went red to blue over
1.0 s with no black sample on schenker and on aspire, NVK's first frame
0.87 and 1.29 s after the request, and back to the Intel GPU over 1.0 and
1.1 s.
`FNL_EVENT_GPU` follows the lift (`events_post_state()`), so a caller or
a round that reads the frame on it reads the frame and not the dissolve;
a hidden window's comes with the first turn it is shown, as every
notification of a hidden window does.
Not for a configure, a resize or a fullscreen toggle: the compositor owns
those and animates them, and the frames stay continuous.

`fnl_set_transition_ms()` sets the one length, 250 ms until it does, 0
the cut there was; the testbed takes `--transition=MS`. At 0 the last
frame is still read back, a frame's wait, since it is what the gap
shows where the library paints it, and a black there would be seen
before the swapchain goes where a driver presents through GDI (the
guests' lavapipe); it is never drawn over the new device's frames. The figure is
the length desktops give a window's own transitions -- GNOME's and
macOS's animate a window in and out in 200 to 300 ms -- and the window
is the thing that changes here: under about 150 ms a dissolve reads as a
flicker between two frames, past about 400 the new device looks slow to
arrive, and the move itself is already a second or more where it is
slow. It is a knob and not a constant for a caller who disagrees.

**Measured** (2026-10-24, release, this desktop, `tools/probe/dissolve.c`
under `tools/probe/dissolve.py`: a window whose area is red on the GPU it
opened on and blue on any other, a bar walking across it, the window
captured by `import` some ten times a second across a move and back). On
X11 with Vulkan, RX 590 to lavapipe at 600 ms: the area's mean went from
red (242, 42, 30) through eleven captures of purple to blue (30, 55, 242)
over 0.55 s, no capture black and none missing, and the GPU event 0.70 s
after the request with 38 frames drawn meanwhile; the way back at
1500 ms, blue to red over 1.5 s in twenty-four steps, the snapshot's bar
standing at one column while the cover was strong and the live bar
walking out from under it as the cover thinned, which is the dissolve
seen. With OpenGL, radeonsi to RADV and back, the same both ways; on
Wayland the same timings, 0.65 to 0.70 s to the event with 37 to 39
frames, the window there not capturable. On schenker's console, the
window on Intel's display path asked for the NVIDIA GPU on nouveau: the
frame read back, the renderer drawn across built on the copy path in 23
ms with the output not handed over, nouveau bound in 219 ms, NVK landed
in 539 ms, and the event 2.0 s after the request -- two dissolves of a
second, the cross's and the landing's -- with 78 frames drawn meanwhile;
back to Intel's display in 1.1 s with 63 frames. The display itself,
sampled there as root some ten times a second (`kms_grab.py` under
`tools/probe/screen_means.py`): red through the take-back of Intel's
display and the cross -- which is Intel's GPU again, so that dissolve is
red to red -- then red to blue in eleven steps over 0.96 s as NVK landed,
and blue to red over 0.98 s on the way back: sixty samples with the
window up, none black, none missing, none further than 0.27 s from the
one before. Aspire the same: nouveau bound in 777 ms, NVK landed in 309,
the event 3.0 s after the request with 115 frames drawn meanwhile, back
in 1.3 s with 62, and its display red to blue in ten steps over 0.96 s
and back over 1.06 s, five hundred samples and none black. The
ThinkPad's console, lavapipe's Vulkan to llvmpipe's OpenGL and back by
the copy path, at a dissolve of 1.5 s: red to blue and back in three to
four samples each, the grab there taking 0.7 s a frame, none black. That
is the blank the whole section is about, gone. And weston, on schenker,
Intel's GPU to NVK and back under a 1.5 s dissolve, read by weston's own
screenshooter eight times a second since the KMS grab cannot read its
tiled framebuffer: the window red until the dissolve began, thirteen
purple blends to blue, and the same back, not one picture of weston's
background between the old surface's last frame and the new one's first
-- so weston keeps a destroyed buffer on screen as GNOME does, and the
old subsurface held under the new one holds its frame there.

**Measured on Windows** (2026-10-24, release, both laptops and both
guests, the same probe reading the screen from inside: a thread of its
own blits the presentable area off the screen DC at the desktop's own
pace, 16.7 ms a sample, and classes the pixels, with the bar's column as
the sign of a frozen frame). What a capture between the move and the
first frame was owed to show: the child window's erase holds the frame
read back from the request to the first frame. It does, on the HD 630,
the GTX 1050 Ti, the HD 530 and the GTX 960M under Vulkan and under
Direct3D 12, and on WARP and lavapipe in the guests -- the bar standing
at its last live column from 20 to 55 ms after the request, which is the
readback and the renew, to the first frame on the new device, 0.3 to
1.2 s later; not one sample black, none further than 25 ms from the one
before (60 ms in the guests), and after the first new frame not one of
the old colour. Two things the same capture found and the rounds, which
read the echo, had not. The erase came from the move *before*: with the
mark that ends it cleared on the render thread after the renew returned,
and the child's first paint left to its queue, under Direct3D 12 the
fresh frame never painted on either laptop -- the window showed the first
move's frame, bar and all, until the first frame of the second move, 300
to 750 ms; under Vulkan the paint came 50 to 70 ms after the old child's
swapchain went, and the first move's frame showed in between for one to
four composed frames. Marked and painted on the window's thread as the
child is made, before the old one goes, neither shows: the fresh frame
stands from the renew to the first frame. And the Direct3D 12
cover drew nothing, the weight being in a channel its blend factor does
not read (above), so every move there cut from the frozen frame to the
new device's first frame; with the weight in all four channels it
dissolves as Vulkan's does. The dissolve as the screen shows it, after
both: on schenker the first frame on the GTX 1050 Ti presented 0.36 s
after the request under Vulkan and 0.79 s under Direct3D 12, the screen
leaving pure red 155 and 80 ms after that and reaching pure blue 150 ms
later still; back on the HD 630 the ramp runs 167 ms on screen under
either. Aspire the same under Direct3D 12 (the screen 60 ms behind the
first frame on the GTX 960M, the ramp 180 ms); under Vulkan the 960M's
second frame waits 205 ms in its acquire and the screen shows the new
device 220 ms after its first frame, when the lift is nine tenths done,
so two frames of the ramp's tail are all that shows. The guests: WARP's
first frame is on screen 76 ms after its present and the ramp runs 185
ms; lavapipe draws its first frame in 530 ms at that size in the guest,
so the lift there is one frame and a cut, as a 250 ms dissolve on a
2 fps device must be. Driven after both fixes, the same evening: both
laptops' Windows, Vulkan and Direct3D 12 on each GPU, 92 checks each on
schenker and 91 on aspire, none failed; both guests with each, 87 each,
none failed -- one `win11` Direct3D 12 round stopped before any move, in
its pointer-capture section, the window cycling through heights of 878,
358 and 5 points until the harness lost it, and passed when run again.

**Driven** (2026-10-24, release): every environment of this desktop and
the two guests, the GPU box's moves across and back in each -- X11 and
Wayland with Vulkan and OpenGL (101, 101, 73 and 73 checks), wine on X11
and on Wayland with Vulkan and Direct3D 12 (85, 54, 88 and 57), and
`win10` and `win11` with each (87 each) -- none failed, each move's
`EV gpu` arriving after its dissolve within the round's wait. The Pi's
console with Vulkan and with OpenGL, 58 checks each, the moves across
V3D, lavapipe and llvmpipe's OpenGL and back, none failed. The laptops
on their live keys: schenker's console on nouveau and on NVIDIA's driver
(72 checks each, the GPU box's move to the other kernel driver and back
in each, drawn across on the copy path), its console with OpenGL on
nouveau (64), its bare X11 with Vulkan and OpenGL (91 and 91) and weston
(62); aspire's console on each driver (72 and 72); the ThinkPad's bare
X11, weston and console on lavapipe (69, 46 and 55) -- none failed, once
the harness knew the copy path's line for what it is. Aspire's Windows,
Vulkan and Direct3D 12 on the HD 530 and the GTX 960M, 91 checks each,
the moves across the two GPUs, WARP and the other renderer in each: none
failed but one round's restore-from-minimize rate, which read under the
busy floor once and 22 % against 14 % visible when run again -- that
device's known stall, and nothing of a move. Schenker's Windows the
same four, 92 checks each, none failed. The Mac, Metal to Vulkan on
MoltenVK and back and the other way round, 77 checks each, none failed:
the drawable blitted into the snapshot and the cover drawn from it under
Metal, with the layer no longer framebuffer-only. What that costs a
Metal frame: `pace.py`'s draw row flat out on the Mac, 338 and 332 fps at
27.0 and 25.8 % of a core with the layer as it is, 333 fps at 27.2 % with
it framebuffer-only, which is nothing the measurement can tell apart.

**Measured across a lift** (2026-10-31, `tools/probe/dissolve.c`, its third
argument asking for the way back that far into the first move's lift;
schenker, Intel's HD 630 to NVK on the GTX 1050 Ti and back, a 1 s
dissolve, `FNL_GPU_RANK=off` so that no calibration moves the window
meanwhile). On its console (the scan-out sampled by `kms_grab.py`), the
way back asked 0.51 s into the lift: the last sample before it (177, 47,
172), the first after the readback (190, 45, 156), then back to red over
1.0 s, the bar live throughout, no black. On its bare Xorg (`import`
through the forwarded socket, five a second) and on its weston (weston's
screenshooter, eight a second) the same: the blend held where it stood
at the readback -- (182, 51, 179) twice on Xorg, (183, 52, 177) on weston
-- then red over 1.0 and 1.1 s, Intel's first frame 0.24 and 0.05 s after
the way back, no picture black and none of weston's background. A loss
during the lift (`gc`, `FNL_FAULT=device-lost@140`, some 0.6 s in) on
both: the blend held through the rebuild, the bar still, then the lift
went on to blue from where its clock stood -- no black, nothing back to
red -- which is the "where the window server kept the last frame, the lift
goes on" of the rule above. A move that will not build on weston
(`FNL_FAULT=create-surface@1`): the last frame up 0.5 s with the bar
standing, then live again on Intel, never a hole. OpenGL to OpenGL on
the console (`FNL_RENDERER=gl`), NVIDIA's OpenGL onto llvmpipe and back,
both the copy path onto the Intel panel: each a 1 s dissolve with the bar
live and no black sample. NVK's landing on aspire's console (the kernel
driver move, the scene moving): NVK's first frame 1.28 s after the
request, then red to blue in nine samples over 0.95 s and back over 0.8
s, none of 58 black. A loss during the lift on the console's display
path (`gc`, schenker, NVK's copy path to the HD 630 on its own panel, a
3 s dissolve, `device-lost@150` about a second in, at (129, 50, 208)):
the next three samples, 0.2 s of the rebuild, are the whole cover --
the frame read back before the move, red, its bar where it stood -- and
the lift then runs again from it to blue over 3.0 s, no black and
nothing older; a loss just after the lift showed one black sample, the
take-back, and never the first move's frame. And a mode change after a
move shows black, not the frame read back for it (the Pi's forced
HDMI-A-1, six modes; V3D to llvmpipe and back, then eight changes
between 1920x1080 and 1024x768): the two gaps a sample fell in were
black, the live frame at the new mode in the next, and no sample of the
way back's frame or a frozen one.

On both laptops' Windows (the same day, both renderers, Intel's GPU to
NVIDIA's and back, the screen read from inside some sixty times a
second): a second move shows nothing of the first's frame -- the gap
holds the frame on screen at the request, the bar frozen, until the
Intel GPU's first frame, no black; a way back asked during the lift holds
the blend where it stood, (186, 51, 175) through 0.65 s of the HD 630's
Direct3D 12 build, then red over 0.9 s; and a loss during the lift (`gc`,
`device-lost`) holds the frame at the loss through the rebuild, 1.0 and
1.35 s on schenker, then takes the lift again from the whole cover --
the frame read back before the move, its bar where it was -- to blue over
a second. A loss outside a transition is not black there either: DWM
keeps the child's last presented frame, the latest and never an older
one, frozen through the rebuild, so the hold's black is what a driver
presenting through GDI shows (the guests' lavapipe, below) and not a
GPU's flip-model swapchain. A minimized window's move is made at once
and `EV gpu` comes with the first frame after the restore, which is the
new GPU's, DWM's restore fading from the desktop straight to it -- but
for Vulkan on aspire's GTX 960M, whose own swapchain's frames reach the
Intel panel a refresh or more late: the fade goes to black for 0.08 s
first, the new swapchain's empty surface.

Not the child's erase, so no hold covers it (2026-11-01, aspire, the same
probe): the first frame shown, read back and painted under the child as
it was drawn, did not change a sample -- the fade still went to black, 11
samples -- and blue came when the second acquire returned, 0.21 s after
the first frame, as without it. Both GPUs' own Vulkan swapchains hold the
first frame through the restore so: on the HD 530 the area keeps the
desktop until then and cuts to the frame. Through DXGI, the 960M's
default way, the second frame is 4 ms after the first and the restore
fades into it. A move made while the window is minimized builds NVIDIA's
own swapchain at the minimized window's stub, 160x28, which the surface
clamps it to; the first frame shown is drawn into it, said suboptimal,
and the next is rebuilt, and none of it is seen.

The GTX 960M's Vulkan under Windows, again (2026-10-31, aspire, the same
probe, from the 960M's first frame to the first sample that differs from
the frozen frame): on its own swapchain 271 ms behind at a 1000 ms
dissolve and 247 ms at 250 ms, the second frame waiting some 200 ms in
its acquire, as on 2026-10-24 (220 and 205 ms); through DXGI, the
default, 107 and 46 ms, and no wait. Now that the lift starts at the
first present the whole of it is seen, a quarter second late on the own
swapchain -- the 250 ms ramp fifteen blend samples -- where on
2026-10-24 the same delay hid nine tenths of it. No sample black in any.

**A loss waits for the queue under Direct3D 12, as under Vulkan.** On
aspire a loss (`gc`, `device-lost`) ended the program six runs of seven:
an exception 0x87D the debug layer raises for "CORRUPTION: ... referenced
by GPU operations in-flight on Command Queue", and nothing catches --
read from two crash dumps, the first an `ID3D12Resource` released by
`destroy_device_objects()`, the second a pipeline state released by the
testbed's `on_gpu_lost`. Both waits for the queue were skipped once the
device was said lost; but a device only said so -- the fault site, a
present that failed on a live device -- still has frames in flight, and
on aspire the Direct3D 11 copy onto Intel's GPU kept more of them than
schenker's did. The renderer now waits whenever it has a device
(`d3d_seam_wait_idle()`): a removed one answers at once, since the wait
asks the device's removed reason. Measured after: 29 losses of 29 and 30
of 30 rebuilt on aspire's GTX 960M and HD 530 (`device-lost%25`, 150 s
each), and `win64-d3d12-native@aspire` on each GPU, 98 checks, none
failed.

On GNOME (the same day, this desktop, the Screenshot portal's pictures,
some three seconds apart). A move that will not build
(`FNL_FAULT=create-surface@1`) is taken back with the window whole and
its bar walking in both pictures, the first across the fault, never a
hole. A minimized window's move is made at once -- lavapipe's first
frame 0.08 s after the request, inside the minimize animation, while
GNOME still invited frames -- and its `EV gpu` comes on the next turn,
3.6 s on, the window still minimized: nothing tells a Wayland client it
is hidden, so the event waits for a turn, not for the window to be
shown. Its restore wants a click on the dock (GNOME's Eval is off, and a
client cannot unminimize itself), which the portal's pointer gives it
(2026-11-01, a probe of `tools/probe/dissolve.c`'s kind that minimizes
its window as it asks the move: its new icon found by differencing the
dock before the start and with the window minimized, and clicked, no
key sent). Moved to lavapipe while minimized and
restored 4.4 s after the move's `EV gpu`, the window's first picture
after the click is lavapipe's frame, whole, its bar walking, and the
ten after it the same until the way back turns it red: no picture of
the twelve has a black pixel inside the window. The pictures are some
2.4 s apart, so the restore's own animation is not sampled. OpenGL to
OpenGL here
cannot be asked: OpenGL lists radeonsi alone on this desktop, on Wayland
and on X11, and `FNL_GPU=llvmpipe` finds no GPU by that name, though
EGL offers Mesa's software device: asked for it with
`EGL_EXT_explicit_device`, Mesa 26.0.8 opens this session's Wayland
display on radeonsi instead (a probe of EGL alone, the display's
`EGL_DEVICE_EXT` the hardware device), so it is not the software device's
display and `list_devices()` leaves it out -- with a line now, `GPU left
out: a software device, whose display for this window EGL opens on
another device`. The thinkpad's Mesa 26.2.6 does the same on bare Xorg;
OpenGL to OpenGL has been seen on KMS, whose surfaceless display takes
the device it is asked for. radeonsi's OpenGL to lavapipe's Vulkan, GNOME's
frozen-frame case, shows lavapipe's frames: a picture taken on lavapipe
is the window pure blue.

**Where a driver presents through GDI** (2026-10-31, the same probe
reading the screen from inside, sixteen to twenty-two milliseconds a
sample, transition 0). In both guests, WARP's Direct3D 12 to lavapipe's
Vulkan: the frame read back held, the bar frozen, 585 ms in win11 and
822 in win10, then lavapipe's frames, and back 42 ms; not one sample
black. lavapipe's first frame after a build is never seen, only its
second -- the read-back frame covers the first. A loss on lavapipe there
(`gc`, `FNL_FAULT=device-lost`): black from the next sample, 35 ms on,
through the rebuild, nothing older at any point; whether the black lands
before or after the old swapchain goes is closer than a sample. Under
wine on bare Xorg (thinkpad, wine 11.20) a move with no transition holds
the read-back frame as the guests do (283 ms onto lavapipe, 1080 ms back
through vkd3d's build), but a loss does not show black: the frame on
screen at the loss stands frozen through the rebuild, 0 black of 425
samples, since winex11 shows a child's pixels only through its copy at
each present (`X11DRV_client_surface_present()`), and the hold's black is
painted into the child by GDI, which nothing copies. It is the last frame
and never an older one.

That copy needs Xcomposite, and wine loads it at run time: with no
`libXcomposite.so.1` -- Arch makes `libxcomposite` an optional dependency
of wine, and the bare machines' sticks have not got it -- winex11 never
redirects the child (`client_surface_update_offscreen()`, `if
(usexcomposite)`), copies from an unmapped window and shows nothing of
it. Every frame after a move under wine on bare Xorg is then drawn and
never shown: the window keeps the frame it left, the bar frozen, which
`content_black_pct()` reads as fine, and the "100 % black" of a child
there from the start (*Under wine on Wayland the child is there from the
start*) is the same missing library. Lent this desktop's library for the
one process, the frames walk on lavapipe after the move and the
read-back frame holds through the way back. So `libxcomposite` is in the
bare machines' package set (`bare_driver.DISTROS`, `libxcomposite1` on
Debian), and the round no longer takes a picture with no black in it for
a live one: after each GPU move, where the environment reads pixels, one
full-resolution row across the scene is read twice half a second apart
(`scene_row()`, `LIVE_APART`), and the two must differ while the scene
moves -- the bar is narrower than `content_black_pct()`'s step. With the
library on the thinkpad, `win64-d3d12-bare@thinkpad` passes whole, 73
checks, the picture moving after the move away and back; with it hidden
from one process, every pair after a move reads the same.

### Composite alpha, and what pre-multiplied costs the blend state

The swapchain's composite alpha follows the grab ring, and is therefore decided
per build — the ring comes and goes with fullscreen, which forces a rebuild
anyway. Windowed on Wayland, the one backend with a ring, the ring is
transparent and the surface cannot claim to be opaque; fullscreen, or on
any other backend, there is no ring and every pixel is opaque.

Saying so matters beyond accuracy. A surface that declares itself opaque and
covers the output can be handed straight to a scanout plane, while one declaring
pre-multiplied alpha has to be blended. Claiming alpha that is not used asks
the compositor to keep a fullscreen window off the fast path for the sake of six
points of ring that are not there. The symptom to recognise is a window that
grows, snaps back and grows again going fullscreen — and that stops the moment
anything captures it, because recording a monitor forces the compositor to
composite.

The margin is therefore set before the *first* build too, not left at zero for
the render loop to correct.

Declaring pre-multiplied alpha makes the alpha channel the window's own
transparency rather than a spare byte, and **every pipeline that writes the
framebuffer has to keep it that way**. The factor to want is
`ONE`/`ONE_MINUS_SRC_ALPHA`: alpha composited "over", exactly as colour is.
Inside the window the destination is 1, so whatever the coverage puts in,
`1 - src.a` puts back; over the ring's zero it leaves the coverage, matching the
colour the blend has already multiplied by it.

`ONE`/`ZERO` *replaces* the destination with the fragment's alpha, and a
stroke's alpha is its coverage — so every antialiased fringe stamps a hole in
the window, deepest where the stroke covered least. It hides behind the opaque
region, which a compositor that honours it never blends through, so the window
looks right on the path that uses it and wrong on any path that goes by the
buffer alone. The signature to recognise is a stroke that is *too bright where
it is faintest*: that is an alpha bug, not a coverage bug.

The subpixel text path needs the same care one step removed. Its first output is
alpha 1 — there is no single coverage to put there, which is the point of the
second output — so the factors are `SRC1_ALPHA` / `ONE_MINUS_SRC1_ALPHA`, and
`shaders/text.frag` puts the pixel's overall coverage in the second output's
alpha for exactly this. The grayscale fallback writes coverage like the strokes
do.

### Configure handshake

xdg-shell's contract is that the commit following an `ack_configure` carries a
buffer of the acked size. The configure handler therefore only latches the
serial: the render thread acks it immediately before the present whose commit
carries that size. Acking on the event instead answers every configure with
whatever had already been drawn — continuously wrong during a resize, which is
what makes a window trail the pointer.

The toplevel states are double-buffered the same way. `xdg_toplevel.configure`
only records them; `xdg_surface.configure` applies them and publishes them to
the render thread in the same locked update as the size, so a frame can never
pair one configure's size with another's decoration state.

A size the app asked for itself is a special case. The compositor answers a
self-chosen size with a configure echoing the size it still has, and adopting
that would undo the request before the buffer carrying it is drawn — so the
request stands for exactly *that one configure*, identified by the size it
carries (`req_from_w`/`req_from_h`, in the Wayland backend's own
`BackendState`: no other window server has the exchange). Taking "the next one"
instead is wrong in a way only this compositor shows: mutter answers a
self-chosen size with **no configure at all**, so the request sits pending
indefinitely and is spent on whatever arrives next — a focus change or leaving
fullscreen resurrecting a size the user had already replaced.

Only a floating window may choose its own size. Maximized, the compositor owns
it and would answer with the maximized size again, but not before the client had
rebuilt its swapchain for the size it asked for and committed a buffer of it.

### Where the time actually goes

`--frame-times`, a line each half second (`FRAME_TIMES_WINDOW`), on a clock of
its own; its `fps` is the counter's latest (`FPS_WINDOW`, 0.2 s):

```
frame 16.66 ms: pace 11.41 fence 0.20 acquire 4.79 record 0.15 submit 0.09 present 0.02 rebuild 0.00 other 0.00 (draw 5.25, hold 11.24, 60 fps, gpu idle 31/31, miss 0/31, period 16.66)
```

Six phases, partitioning the loop between consecutive turns, the second
reported in its two parts, and what is left over:

* **pace** — the loop's own throttle: the invitation on Wayland, the late-latch
  hold, nothing at all in a tearing mode.
* **wait** — blocked on the device. Every device wait the frame takes, which
  here is three: `vkWaitForFences` on the frame slot, `vkAcquireNextImageKHR`,
  and the per-image fence before a re-record. **This is where the GPU's own time
  appears**, because it is where the CPU sits out work already submitted — and
  it is the number the instrument exists for, since a frame rate cannot
  distinguish a slow GPU from a slow anything else. That last device wait sits
  inside the re-record branch and must still be counted here rather than in
  `record`: a device wait reported as recording is the instrument saying the CPU
  was busy when it was in fact blocked, and it grows exactly the phase a
  GPU-bound reading should be shrinking.

  The line gives it in two parts, **fence** and **acquire**, because one
  figure for both could not tell a GPU behind from a presentation engine
  behind, and on schenker's weston that was the question. `fence` is the
  device's fences -- `App::device_block`, which every renderer sets: the
  slot's fence, and Vulkan's per-image one -- and `acquire` the rest: the
  swapchain image on Vulkan, the frame-latency object on Direct3D 12,
  nothing on OpenGL, which has neither. Lines from before the split read
  `wait`, the two summed.
* **record** — building the command buffer, and zero on every frame the
  recording cache serves.
* **submit** — handing it to the queue: `vkResetFences`, `vkQueueSubmit`, and
  the ack and frame request that ride on the present's commit.
* **present** — the present call itself.
* **rebuild** — making a swapchain and, turns later, retiring the one before
  it, spread over the second's frames like the rest. Zero on a window nobody
  is resizing. It has a column because without one a resize on a slow driver
  is a 28 ms frame whose five parts sum to four.
* **other** — the frame less `pace`, `draw` and `rebuild`, never below
  zero: the loop's time outside every part, a turn that drew nothing or
  the wait for the next size. Named for the same reason as `rebuild`, so
  that parts which do not add up to the frame say how far they fall short.

**The same figures as totals: `fnl_frame_stats()`.** The line is a
diagnostic -- text, averaged over a window that restarts at every rest, and
only while `frame_times` was asked for at init. A caller measuring itself
wants numbers it can subtract across exactly the span it measures, so the
loop also keeps running totals (`FnlFrameStats`, microseconds), counted at
the same sites from the first `fnl_set_frame_stats(true)` on and never
reset, copied out under `App::lock` once a drawn frame. Every clock read the
two need is gated on one flag set at the top of each turn, `App::timing`
(`frame_times` or the stats on), so the renderers' sites changed from
`frame_times` to it and nothing is read while both are off. Measured
against the line on schenker over the same five seconds, each phase's mean
agrees within 0.01 ms on Xorg and KMS and 0.2 ms on weston, whose windows of
half a second do not line up with the measure; and with the totals on, the
testbed's flat-out rate on Xorg is the same as without them, 1403 to 1437
fps against 1436 to 1445.

**`submit` is separate from `record` because one number was answering both
questions, and it answered neither.** Measured together the pair reads
`0.08 ms` on a window that records nothing at all — so `record` named a floor
that no amount of recording less could remove, and the 0.135 ms an actual
re-record costs was three quarters of a figure that never went to zero. Apart,
an idle window reads `record 0.00 submit 0.09` and a moving one `record 0.14
submit 0.09`: what the recording cache is worth, stated, and what a submit
costs whatever it is serving. The split needs no third clock read — `submit` is
the rest of the span `late_cost` is already measured over.

`hold` is not a phase of its own but a *part* of `pace`: the late-latch delay
currently in force, and zero in a tearing mode. It is reported because it is
otherwise the one figure in that mechanism with nothing to observe it by — a
delay doing its job and a delay doing nothing both look like a frame that
arrived on time.

Two counters, because the phases alone are ambiguous in exactly the ways that
matter.

**`gpu idle`** is how many frames arrived to find the device already drained. A
GPU that is genuinely busy and a GPU that finished long ago while the
presentation engine took its time are the same call blocking for the same
duration. It reads the status of the fence *this* frame is about to wait on —
the frame two back signals it — so it means "this frame will not have to wait
for the device", and it is a non-blocking read of a fence that is already there.
Sampling the *previous* slot's fence instead answers no whatever the device is
doing, since a pipelined renderer has that frame in flight by construction.
`idle 0/n` with a large `wait` is a GPU that really is taking that long;
`idle n/n` with a large `wait` is a present that will not retire.

**`miss`** is the counter `hold` cannot be read without: cycles missed over
frames measured. The hold buys freshness and the only thing it can cost is a
missed cycle. It is deliberately *not* the probe's own estimate — that exists
only on the frames the probe runs on, so it would vanish under
`--no-late-latch`, which is the floor the rest have to be read against. It needs
a signal quantised to the display, and the invitation is the only one the loop
has, so it reads `0/0` on every backend but Wayland and says so. The present's
own spacing does not substitute: a FIFO gap slides smoothly instead of doubling
once the queue is drained, so it would report no misses and be wrong.

`draw` is the whole renderer call and `frame` the whole turn; the two gaps —
`draw` minus the four phases inside it, and `frame` minus `pace` and `draw` —
are the loop's own overhead, and reading as nearly nothing is what says the
phases are drawn in the right places. Summed and divided rather than smoothed,
so the figure is a plain mean over the reporting window.

Off by default: a diagnostic, not a metric.

**Whose instructions they are.** Counted with callgrind, which needs no
privilege where `perf` does, over 90 s of `fenestal-pace` resizing X11 at
30 sizes a second, startup excluded (counters zeroed once the window was
up). `render_loop` is 83% of everything inclusive. Inside it,
`create_swapchain()` is 44% and `draw_frame()` 35%, both nearly all the
driver's -- the new swapchain for each size, and the submit. The library's
own recording is the remainder: the overlay's flush 6%, its text 2%, the
titlebar controls 3%, and the depth image each rebuild makes 3%. Nothing
of the library's own is worth making faster for the CPU; what a resize
costs is a swapchain per size, which is the window server's terms, and the
frame budget is the GPU's and the compositor's (`pace` above).

### The startup inventory, and `--gpu=<n>`

The inventory is logged once the renderer has picked, one line per entry of
the merged list (`inventory_say()`; the renderer named after the kind where
the list spans several):

```
GPU 1: AMD Radeon RX 590 GME (RADV POLARIS10) (discrete)
GPU 2: llvmpipe (LLVM 21.1.8, 256 bits) (software)
```

Only devices that can draw the window are listed -- that present to its surface,
or on KMS reach the output by the copy path -- because only those are indexable;
one that can also present through DXGI on Win32 is listed twice. The type is
printed rather than inferred from the name — a window rendering in software has
an explanation nobody would otherwise look for. Both the automatic candidate and
the indexed one come out of a single pass, so a machine with two devices prints
two lines whichever is asked for.

**NVK before Turing ranks after an integrated GPU.** On schenker (Clevo
P64: HD 630 beside a GTX 1050 Ti) under the live key, Mesa 26.2.4 offers
the NVIDIA GPU through NVK, which the ranking put first. Before Turing the
kernel has no GSP firmware to reclock with, so the GPU runs at its boot
clocks -- the testbed with its scene moving drew 24 fps there against 60 on
the HD 630 -- and NVK on that Pascal garbles every glyph for good after
maximize, fullscreen, back and restore: each glyph in its place and
colour, its texels shifted in blocks, as if the atlas were read with
another layout. The sequence was not the cause but its pauses: nouveau
runtime-suspends the discrete GPU after 5 s idle, and an optimal-tiling
image left in VRAM comes back from the resume with its texels in the
wrong places. `tools/probe/nvkatlas.c` shows it with nothing else -- an
image uploaded, 12 s idle, 261,090 of its 524,288 texels wrong -- and
never with `power/control` set to `on`, with 3 s idle, with
`NVK_DEBUG=gart`, or on the HD 630. The validation layer says nothing
over any of it. That is the kernel's (kernel 7.2.9; the report is drafted
in `docs/mesa-nvk-pascal.md`). `device_passed_over()` tells it by NVK's
own spelling, `(NVK GP107)`, and the PCI ID below Turing's 0x1e00, and
ranks such a device one below integrated, with a line saying so; a name
or an index still chooses it.

Chosen anyway, it keeps the glyph atlas out of VRAM
(`atlas_memory_type()`): the atlas is the one image the library uploads
once and keeps, where every other is drawn each frame. NVK allows the
image all three of its memory types, and the atlas goes in the first that
is not `DEVICE_LOCAL`, system memory (`HOST_VISIBLE|HOST_COHERENT|
HOST_CACHED`). There the probe's image is whole after 12 s and 20 s idle
(`NVKATLAS_HOST=1`), and the testbed's keybindings after 20 s differ from
before in 0 pixels, against 5,392 of 61,200 with the atlas in VRAM. Read
over the bus it costs nothing measured: 4.05 ms a frame uncapped against
4.02. Uploading again after a resume was the alternative, and nothing in
Vulkan says one happened. A device that allows the image no other type
keeps it in VRAM, with a warning. A caller's own images are the caller's.

The resume costs time as well. The first event after an idle long enough
reaches the program in 1.86 s on that GPU, against 44 to 201 ms awake:
the frame that drains it waits for the GPU to power on. The harness waits
for it as for a checker tree, where a bare machine's discrete GPU is left
to runtime-suspend. Under weston it costs nothing more: weston presents
a frame drawn just before the GPU suspends within 40 ms of its fence.
`tools/probe/heldpresent.c` -- Mesa's WSI with nothing of the library,
the fence watched as a sync_file (a status query would itself wake the
GPU and read the fence's seqno) -- found so after the GPU suspended (3
rounds), after a swapchain made anew at another size (3), submitted the
moment `runtime_status` reads `suspending` (4, the acquire waiting 2.1 s
for the resume), and after a maximize or restore with the ack just before
the present (4), 165 ms at 1920x1080 being the copy to the HD 630 at
boot clocks; the fence signals within 2 ms of the submit each time.

Read once as weston holding such frames, and it was not. Weston rounds on
NVK lost the window after the restore from maximized (2026-10-16, "lost
the window at (640, 17)"), and a trace under `WAYLAND_DEBUG=client` showed
no `wl_pointer` event over 27 s of moves meant for the window, keys still
arriving, and two frames' `presented` 1.8 s after a key. Weston had two
outputs then, HDMI-A-1 right of the panel, and the harness's window sweep
went home by one -30000 move, which weston keeps on the output it began
on: once a row ran onto HDMI the pointer stayed there, and the probe's
own move into the window was the same single push. The sweep needed a
second row only because NVK, its GPU off, answered the first hit 1.86 s
late. The `presented` lines were Mesa's: the library asked for no
presentation feedback then, Mesa's WSI asks on a queue of its own that
only its own calls on the render thread dispatch, and `WAYLAND_DEBUG`
prints an event when it is dispatched -- so at the frame the key woke,
after the GPU's resume, and not when weston presented. Both failures and
the trace are that; the sweep goes home in steps that each land on an
output, and traces a late hit back along its row (`wayland_driver.py`,
`_corner()`, `find_window()`). A window lost on weston that is not this
is `heldpresent`'s first (`tools/probe/heldpresent.sh`).

`FnlInitDesc::gpu_index` is a one-based index into that enumeration, which is
**not** the order `pick_device()` ranks them in (where a measured ranking is
kept, `FNL_GPU_RANK`, the index counts the ranked list instead) — the ranking
puts discrete ahead of integrated, and on a hybrid laptop the integrated GPU is
usually enumerated first. An index past the end warns and falls back rather than
failing to open a window: the caller asked for a different GPU, not for none.

The Vulkan side cannot rank a device before there is a surface to ask it
about, so the list does not exist until the window does: before
`fnl_init()` the log is the enumeration, and after it `fnl_gpu_count()` and
`fnl_gpu_name_at()` read it back.

The escape hatch matters more than the ranking. Which GPU a window should
present from is not a question this library can answer for its caller — a heavy
scene wants the fast device and will pay the cross-adapter present, a trivial
one wants the device that already owns the display.

**The index is not one device across programs, so there is a name.**
`NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` are read
out of the **executable's** export table, so a library cannot carry them;
`testbed/main.c` has the two lines and no other program the build makes does.
Measured on a hybrid laptop (GTX 1050 Ti beside the HD 630 that drives the
panel, Windows 11, NVIDIA 582.78), what they move for a Vulkan program is
the *order*:

| program | exports | GPU 1 | GPU 2 | automatic |
|---|---|---|---|---|
| `fenestal-testbed` | yes | NVIDIA | Intel | NVIDIA |
| `fenestal-minimal`, `fenestal-pace` | no | Intel | NVIDIA | NVIDIA |

Eight starts of each gave the same order every time, with the validation
layer in front and with implicit layers off: it is the program and never
the start. The automatic choice is the discrete device either way, because
the ranking does not read the order. So the exports change nothing this
library decides and everything `--gpu=1` means, and an index copied from
one program's log to another's command line is the other device.

`FnlInitDesc::gpu_name` is the choice that holds: any part of the device's
name as the log prints it, ASCII letter case aside, the first device that
carries it. It wins over `gpu_index`, and a name nothing carries warns and
falls back as an index past the end does. With neither set the library
reads `FNL_GPU` from the environment — 1 to 8 is an index, anything else a
name, so `1050` is a card and not the thousand-and-fiftieth — which is how
a program with no option for it is put on the other device, and how a
round is. The caller comes before the environment: a program that chose a
device has a reason the person starting it may not know. Where there was a
choice the log says how it came out, `GPU in use: <name>`.

On this desktop, whose second device is llvmpipe, `FNL_GPU=radeon` under
AddressSanitizer is the round as it always was, 74 checks on X11 and 53 on
Wayland with none failed. `FNL_GPU=llvmpipe` on X11 draws and passes
everything but the exit: 112 bytes in two blocks, allocated on llvmpipe's
own threads with no frame of this tree in the stack, which is that
driver's and not the choice's.

Direct3D 12 ranks as Vulkan does, the most capable first: its automatic
choice is the hardware adapter DXGI 1.6 ranks first for performance
(`EnumAdapterByGpuPreference`, *high performance*) -- the discrete GPU, or
the one Windows' graphics settings name for the program -- while its list,
and so `gpu_index`, keeps `EnumAdapters1`'s order, the display's, which
puts the panel's integrated GPU first. Where DXGI has no such ranking
(before Windows 10 1803) the first hardware adapter is taken. On both
laptops `fenestal-pace` and `fenestal-minimal`, neither with the exports,
drew unforced on the NVIDIA GPU (the GTX 960M, the GTX 1050 Ti), where
`EnumAdapters1`'s first had put them on Intel.

**The pipeline cache is persisted, so shader compiles are paid once.** One
file a device, `fenestal.pipeline_cache-<uuid>` with the device's own
`pipelineCacheUUID`, named for the library rather than the caller: a driver
keys its entries by shader hash, so two Fenestal programs warm the same
cache, and a caller's own pipelines land in it through the interop. A
driver takes only a blob of its own UUID's, so one file for every device
held the last device's alone, and a window moved between NVK and NVIDIA's
driver (*Another kernel driver, with the window open*) began each build
cold and wrote the file at every move. It
lives in `$XDG_CACHE_HOME`, or `$HOME/.cache` where that is unset, and in
`%LOCALAPPDATA%` on Windows; with none of them set there is no file and
nothing else changes. It is written to a temporary file beside the target and
renamed over it, because another program shares the file and a reader must
find the old cache or the new one, never part of either. And it is not written
at all when the blob is what was loaded — its size and a hash are kept from
the read, rather than a copy of what can be megabytes — so a run that compiled
nothing new leaves the file alone. What is read is bounded at 64 MiB before
the driver validates it: the file is only semi-trusted.

## The scene

### Render scale

The surface scale is the desktop's and covers the whole window. This is the
other one, and it is the caller's: **the scene may be rendered at fewer pixels
than the window has and stretched back over it.** The overlay is not — the
titlebar, the panels and the crosshair are drawn at the window's own resolution,
on top.

`fnl_set_scene_size_px()` sets it, which is why a resolution still means
something in fullscreen and maximized: the compositor owns the window there, so
the only thing left for a size to change is how many pixels the scene is shaded
with.

Two paths, picked on one test — is a size pinned *and* smaller than the
presentable area?

* **No.** The scene is drawn straight into the swapchain across the presentable
  area. No offscreen image exists, nothing is sampled.
* **Yes.** A pass of its own renders into the scene image (`VK(a)->scene`) at
  that resolution, and the window's pass draws one triangle over the area
  with that image as its texture. One fetch per screen pixel,
  `VK_FILTER_LINEAR`.

A pin larger than the area on either axis is **ignored, not fitted**: fitting it
means either distorting an aspect nobody asked to distort or picking a size
nobody chose. A pin *equal* to the area is native by another name — the
offscreen image and the fetch would both be identity — so `scene_pin_applies()`
refuses it, which is also the trap in benchmarking this: pinning the area's
exact size quietly measures the direct path twice.

The scene is scaled to fit, centred, aspect kept, so what is left over is the
render pass's own black clear showing as a bar on two sides. Keeping the aspect
is the app's to do precisely because the alternative was not: the other way to
put a 1280x720 signal on a 4K panel is to change the monitor's *mode*, and then
a mismatched aspect belongs to the monitor's own menu.

Changing the mode was considered and turned down. There is no Wayland protocol
for it, so it would be a compositor-specific side channel; on GNOME that means
`org.gnome.Mutter.DisplayConfig` over D-Bus, which is a dependency in a program
whose runtime dependencies are wayland-client and Vulkan, is GNOME-only, costs a
second of black screen each way, reflows every window on the monitor and does
not put them back, and has no revert timer if the process dies. What it would
have bought is scanning out 1280x720 with nothing for the compositor to do;
rendering small and scaling in-app keeps the buffer output-sized, which is what
keeps the surface eligible for direct scanout, and costs a fetch per screen
pixel instead — around 33 MB of writes at 4K.

### Antialiasing

Two modes, set with `fnl_set_antialias()`, and the difference between them is
the point of having both. Both are built on the offscreen target, so asking for
either turns it on even at the window's own resolution — there has to be an
image to resolve into or to read.

Two calls tell before asking, and they are not the same question.
`fnl_offscreen_supported()` covers the render scale and both modes together:
false means none of the three can run on this GPU at all.
`fnl_max_samples()` is FSAA's further condition — 1 means this device offers no
sample count worth having, even where the target exists. A caller's control for
either mode should be drawn dim rather than left to fail quietly.

**`FSAA` — multisampling.** The scene is rendered into a 4× multisampled colour
and depth pair and *resolved* into the image the rest of the frame reads. The
sample count is the best of 4× and 2× the device offers for both attachments at
once (`framebufferColorSampleCounts & framebufferDepthSampleCounts`); where it
offers neither, `fnl_max_samples()` reports 1 and asking for FSAA leaves the
window at `FNL_AA_NONE`.

The resolve is a subpass attachment, not a second pass: `pResolveAttachments`
names the single-sampled image and the hardware averages the samples as the pass
ends. The multisampled colour is never stored, so its store op is `DONT_CARE`
and it asks for transient, lazily allocated memory where the device has any.

Multisampling antialiases *primitive coverage* and nothing else: the rasterizer
takes several coverage samples per pixel, the fragment shader still runs once,
and the resolve averages what the triangle actually covered. That is why the
test pattern's star is drawn as **geometry** — 36 wedges, 216 vertices computed
from `gl_VertexIndex` with nothing bound — while the grid and the ramps are one
covering triangle and a fragment shader, and stay aliased under FSAA on purpose.
The contrast is the demonstration.

The resolve averages in **linear light**, which is worth knowing rather than
assuming: the scene image carries the surface's `_SRGB` format, and a resolve
averaging the stored bytes would be averaging encoded values — the classic way
multisampling comes out subtly wrong. Half coverage between the pattern's wedge
and gap lands where linear averaging predicts and nowhere near where sRGB
averaging would.

**`MLAA` — morphological, after the fact.** A pass at the scene's own resolution
reads the rendered image and writes another: it finds the stair-steps an edge
left behind, works out how much of each pixel the edge really covered, and
blends. It has no geometry and no coverage samples, so it works on everything in
the picture — including what a fragment shader drew, which multisampling cannot
touch.

At the scene's resolution and not the window's, deliberately: the filter looks
for steps in the scene's pixel grid, and run after the upscale it would be
reading a picture whose steps the bilinear stretch had already smeared.

Where it writes depends on whether there is an upscale at all. A scene pinned
smaller than its area is filtered in a pass of its own, into an image of its
own, ahead of the fetch that scales the result. A scene already the size of
the rectangle it lands on is filtered **straight into the window's pass**
(`mlaa_win_pipeline`, when `scene_mlaa_direct()` holds): an intermediate image
there would be fetched back texel for texel into the same format, a
full-resolution write and read that change nothing. So at native size MLAA
costs the filter and no second image.

That it changes nothing is measured and not only argued. The frozen frame
(`scene_time == 0`), taken out of the swapchain by Mesa's screenshot layer
from a build as it stands and from one with `scene_mlaa_direct()` made to
answer false, so that the same frame goes through the pass of its own: on X11
and on Wayland not one pixel differs outside the FPS readout, and both differ
from the unfiltered frame in the same fifteen thousand.

MLAA proper (Reshetov, 2009) is three passes with a precomputed area texture.
This is one pass that does the search directly — from each edge pixel it walks
along the edge in both directions until the edge stops, and takes the coverage
from where the pixel sits in that span. Same trapezoid area, computed rather
than stored, which is what FXAA's quality path does too.

What is lost against the three-pass version: the search is bounded at 12 steps,
the shapes at the ends are not classified, and diagonals get **nothing** — a 45°
edge steps every pixel, so every span is one pixel long and the weight
`clamp(0.5 - (0 + 0.5) / 1, 0, 0.5)` is exactly zero. The effect is graded
rather than a cliff,
degrading monotonically from near-axis edges (where it is *more* accurate than
four coverage samples) to 45° (where it does nothing). Closing that is SMAA's
diagonal-pattern search, and it is a feature rather than a fix.

**A span shorter than four is refused, and that rule is what makes the filter
safe to switch on.** The run's length is the only thing the coverage is derived
from, so a run of one carries no information and runs of two and three carry
almost none — yielding a quarter-strength blend chosen from noise. Where the
star's wedges converge, every span is two or three, and the result is speckle:
a *regular* staircase turned into an irregular one, which the eye reads as noise
where it read the regular one as a line. That is a loss even though the
individual pixels are closer to the truth. Four rather than three or five
because that is about where a staircase stops being visible as one.

An edge that never ends inside the search is left alone rather than blended flat
— a long unbroken step is a real feature of the picture.

`EDGE_THRESHOLD` is compared against **linear** luma, which makes it
perceptually lopsided: 0.045 of linear is an enormous step near black and a tiny
one near white, so the filter cannot see the pattern's dim grid lines and can
see its bright ones. FXAA and SMAA both threshold *relative* to the local
maximum with an absolute floor under it, and that is the change to make if the
filter is ever wanted on darker content. It is not made here because it changes
what the filter produces.

**What they cost.** AMD Radeon RX 590 GME (RADV POLARIS10), the Wayland build
at `--scale=2` — a 2584x1532 buffer over a 2560x1440 presentable area —
uncapped on `MAILBOX`, motion off, command buffer reused (`record 0.00`). The
mode and the pin are set through the public calls at start-up, the figure is
`--frame-times`' frames a second inverted, the median of the last six seconds
of an eight-second run, and every row was taken in one sitting, twice, the two
agreeing to within 0.01 ms:

| the scene is | ms/frame | on top of a direct frame |
|---|---|---|
| drawn straight into the window | 0.241 | — |
| into its own image and fetched back, 1:1 | 0.368 | +0.127 |
| multisampled 4× and resolved into it | 0.539 | +0.298 |
| filtered straight into the window's pass | 0.716 | +0.475 |
| filtered in a pass of its own, then fetched | 0.858 | +0.617 |
| held at 640x480, neither | 0.174 | −0.067 |
| held at 640x480, multisampled | 0.197 | −0.044 |
| held at 640x480, filtered | 0.230 | −0.011 |

The two modes are one setting with three values, so there is no row with both.
The 1:1 rows that go through the fetch are pinned a pixel short of the area,
2560x1439: an equal pin is refused (see *Render scale*) and would measure the
direct path twice. The fifth row is that pin with MLAA on, which is the only
way to put a full-size scene through the filter's own pass.

Four things fall out of that. The offscreen round trip is **not free** — a
full-resolution write the direct path never makes and a full-resolution bilinear
read after it — and at this size that is 53% on top of a direct frame before
either mode has done anything. Against that baseline multisampling costs +0.171
ms and the morphological filter +0.348 ms where it writes the window directly.
Filtering into an image of its own and fetching that back costs +0.490 ms
instead, so the direct route is worth 0.142 ms a frame at this size, which is
the copy it does not make. And the render scale is by a wide margin the most
effective of the three: either mode at 640x480 costs less than a direct frame
at full resolution, let alone the *unfiltered* offscreen path.

What is left on the table is inside `mlaa.frag`: the search takes two texture
fetches per step per direction (every one `textureLod` at level 0, since most
sit after an early return or in a loop, where an implicit derivative is
undefined), where FXAA's quality path samples the midpoint
between the two rows with a single bilinear tap and halves that. Not taken,
because it changes what the filter produces.

**The offscreen target is the one allocation a user gesture can make arbitrarily
large, so each part is built only when something asks for it, and degrades
rather than dies.** What comes with the size is the scene image, its depth and
their framebuffer — what every offscreen frame needs whatever the mode. The
multisampled pair and MLAA's image are built by `scene_ensure_aa()` the first
time a frame wants them: at a 4K scene they are 276 MB and 34 MB — the
driver's own memory requirement for a 3840x2160 4× colour and depth pair,
142.1 and 134.3 MB, and for one more colour image — and a window that never
ticks either box does not carry them. The build runs on the render
thread ahead of the record decision, and bumps the record generation, because
whatever is cached was recorded without the new image. It needs no queue
drain — nothing recorded so far names an object that did not exist.

What is built stays until the scene's resolution next changes. Destroying is
the half that needs the queue idle, and a click on a control is not worth
that, so a mode switched off keeps its images.

A device that refuses the multisampled pair costs FSAA alone, and one that
refuses MLAA's image costs MLAA alone; the rest of the target stands. A
refusal of the scene image or its depth costs the whole target and the scene
is drawn straight into the window, which is the frame it would have drawn with
neither mode on. Every refusal is one line through the log sink, and a refused
mode is remembered until the target is replaced, so a device with no room is
asked once per target rather than once per frame. The caller's request stands
(it is the request, not the answer), and the status line reports what actually
happened, because the reporting calls ask the objects rather than the request.

### A pipeline per target

The scene's shaders are built once for each of the three places the scene can be
drawn — the window's pass, the offscreen pass, and the multisampled one. A
pipeline may only be bound inside a render pass *compatible* with the one it was
built against, and compatibility covers the sample count and — by the letter of
the spec, and as the validation layer enforces it — the subpass dependencies,
which the three deliberately disagree on: the offscreen passes have to order the
fetch that follows them against the writes they just made. Same shaders, same
state, same cache entry; it costs a handle apiece.

`FnlSceneTarget` is public for exactly this reason: a caller has to build one
pipeline per target at `on_gpu_created` time and index them by what `FnlFrame`
reports. Which one a frame uses is the library's to decide.

The scene's depth image is a single image, not one per swapchain image — the
scene is drawn once a frame into one target — so the clear at the head of its
pass aliases the writes at the tail of the last frame's. That needs the *write*
half of its external dependency as well as the read half; leaving it out is a
`SYNC-HAZARD-WRITE-AFTER-WRITE` on the layout transition, once a frame.

The swapchain's own depth attachment is likewise **one image for the whole
swapchain**, named by every framebuffer, and it is the same arrangement that
makes it safe: the window's pass clears it and never stores it, so no frame
reads what another left, and the pass's external dependency names the depth
stages and the depth write on both sides, which orders each frame's clear after
the last frame's writes. One per swapchain image is the alternative, and it buys
nothing a cleared, never-stored attachment needs while costing the image count
in memory: a 2584x1532 image at `D32_SFLOAT` is 17,596,424 bytes by the driver's
own memory requirement — 16.8 MiB, a tenth more than the texels alone — so three
are 50.3 MiB against the one. It asks for transient memory where the device has
any. The per-frame clear is free: 0.2412 ms a frame against 0.2399 with the
load op flipped to `DONT_CARE`, three runs of each under the conditions of the
table in *Antialiasing*, and a difference inside their own spread — a
hierarchical-Z fast clear is what it should be.

### Colour

Overlay colours are authored in sRGB, named by an enum in `app.h`, and converted
once into `App::pal` by `app_palette_init()` as soon as the surface format is
resolved. When that format is an `_SRGB` one the hardware encodes linear → sRGB
on write, so the palette is linearised first; on a UNORM surface it passes
through unchanged. A caller's colours do not come through the palette at all —
they arrive as `FnlColor` per call and are converted by `api_overlay.c`.

The surface format is chosen by **preference order** rather than by one name.
Asking only for `B8G8R8A8_SRGB` while the `srgb_surface` test recognises three
different `_SRGB` formats means a surface offering one of the other two, and no
BGRA, falls back to `formats[0]` and quietly takes the UNORM path — palette
unlinearised, text shader compensating for a blend space it is not in,
everything a shade off.

**One path takes it by design: Vulkan through DXGI on Windows** (*Vulkan
through DXGI*), whose images are Direct3D 11 textures in
`B8G8R8A8_UNORM` and are drawn in that format (`surfaceless_format()`),
so `srgb_surface` is false there and the overlay is the UNORM fallback's,
while Direct3D 12 draws the same flip-model buffers through `_SRGB`
views. The caller's scene layout says `B8G8R8A8_UNORM` there, and a scene
encodes its own output, as `scene.frag` does under `pc.srgb == 0`.

`testbed/shaders/scene.frag` linearises the colour it authors
**unconditionally**, composites in linear, and encodes on the way out only when
the hardware is not going to:

```glsl
vec3 col = lin(c);                                      /* always */
out_color = vec4(pc.srgb != 0u ? col : enc(col), 1.0);
```

The obvious arrangement is the other one — linearise only when the surface will
re-encode. For a scene that *generates* every colour it draws the two are
exactly equivalent, because `enc(lin(c)) == c`. They stop being equivalent the
moment the scene **samples** anything: a sampler on an `_SRGB` image applies the
EOTF whatever the swapchain is, so `texture()` hands back linear light on both
paths, and the input-conditional rule would then composite linear samples
against sRGB-encoded generated colour. sRGB 0.5 is linear 0.214, and treating
0.214 as already encoded puts about a fifth of the light on the panel.

The reason to write it the correct way round while it makes no difference is
that the other way is a trap with no tripwire: it passes every check this
project has for as long as the scene stays procedural, and springs silently for
whoever adds the first texture. `blit.frag` and `mlaa.frag` need nothing either
way — they work in linear on an `_SRGB` surface and in encoded values on a UNORM
one. `scene.frag`'s output is the only boundary that moves.

### The test pattern

The scene has to draw *something* or none of the above is visible.
`testbed/shaders/scene.frag` is laid out in **scene pixels**, so what
is on screen is what the upscale made of it:

| | shows |
|---|---|
| one-pixel grid every 32, brighter every 128 | the filter, and the pixel grid itself |
| 36-wedge star, as geometry | where a resampler fails first, and the only thing FSAA can touch |
| grey ramp, 16-step ramp, six primaries at two levels | banding, and the transfer function |
| two-pixel border at the scene's edge | the scene's rectangle, so letterboxing is visible as a gap |

It is **still** by default and `F2` sets it moving. Still by default because a
window whose draw state does not change re-records nothing and re-presents the
command buffer it already has. The animation clock lives in `FnlDrawState`, so
switching motion on is what breaks that comparison and switching it off restores
it — and `scene_time == 0` is the one instant reproducible across runs and
machines, which is what makes a captured frame something a later capture can be
compared against.

## Text

The overlay is set in Ubuntu Sans Mono, with Ubuntu Sans baked alongside it for
UI chrome. `tools/mkfont` runs FreeType **during the build** and bakes coverage
atlases — one per surface scale in its `SCALES` table, each rasterised at that
scale's true ppem. The atlases go into `.rodata` as a binary blob via an
`.incbin` stub; the program picks the atlas nearest the scale in force, uploads
that one, and places quads. Nothing is rasterised at run time and FreeType is
not linked into the executable.

### Why one atlas per scale

At a ten-pixel cap height a stem is about one pixel wide and a crossbar about
one pixel thick, so what reaches the eye is a handful of coverage values and the
result depends entirely on grid-fitting the outline before filling it. That is
only valid for the pixel grid it was computed for — a big atlas filtered down is
the thing that reads wrong.

The cost is a surface on a scale not in the table: the glyphs come from the
nearest atlas while the pen steps are computed at the real scale, so a line sits
very slightly loose in its column. Between two rungs that is a few per cent.

**Past the ends of the ladder the rule changes**, because there is no near rung
to be slightly wrong about: the 2.0 atlas asked to serve a 3.0 surface draws
every glyph at two thirds the size the layout steps at. Cap height, which should
measure 10 pixels at any scale, came out 8 at 2.5x, 7 at 3x and 5 at 4x. So
beyond either end the text appender (`text_resampled()`, asked by
`src/core/overlay.c`) scales the quad to the size the layout wants — soft
rather than crisp, and the right size, which is the one thing worth trading
crisp edges for.

Scaling the quad also switches the subpixel path off, by pushing a texel step of
zero. Its three samples are the pixel's three emitters only while one texel
really is a third of a pixel across; a scaled quad breaks that and the fringes
would colour the text rather than sharpen it. At a step of zero all three
samples fall on the middle texel, which the FIR5 filter already made the
weighted average of the three — so the shader's own arithmetic yields the
grayscale answer without a branch for it.

### Three details carry most of the quality

`tools/verifyfont` reproduces the bake and compares it texel by texel. The
two share the rasteriser — `tools/font_raster.h`, FreeType in and coverage
out, where they must agree to the bit — and nothing after it: where a cell
sits, which ranges a size carries and what borrows from what are worked out
twice, since a check that shared that arithmetic would prove nothing.

Every cell in the table is accounted for as one of four things — compared,
blank, absent by design, or failed — and the check fails on a glyph a size is
baked for that FreeType cannot load or render, on a glyph index no code point
reaches, and when the four do not add up to the table or nothing was compared
at all: a check that compared nothing has not passed. On the current bake it
reads `29178 cells: 6780 compared, 60 blank, 22338 absent by design, 0
failed`.

* **The outline is grid-aligned before it is filled.** The face's own hinting
  fits crossbars to a row but leaves vertical stems wherever the outline falls,
  so a one-pixel stem straddles two columns and reads as a grey smear. Shifting
  the outline left by the fraction its left edge sits above a pixel boundary
  takes the strongest stem column from 0.808 to 0.894 coverage. FreeType's
  autohinter snaps stems too and scores marginally better, but redraws the face
  6% heavier and 10% wider — enough to overflow the monospace pen step.
* **Coverage is never run through a transfer function.** It is a geometric area
  fraction, not a light level. On an `_SRGB` surface the hardware already blends
  in linear light, so the weight it wants is the coverage untouched; the UNORM
  fallback wants it *encoded* (`shaders/text.frag` derives why).
* **Subpixel antialiasing** where the device offers `dualSrcBlend`. The atlas is
  rendered at `FT_RENDER_MODE_LCD` through FreeType's FIR5 filter — three
  samples per pixel, one per emitter — and the blend takes a separate weight per
  channel from a second fragment output. Without the feature the same shader
  compiles to a grayscale variant reading the middle of each pixel's three
  texels. `BGR` in `shaders/text.frag` is the one thing the program cannot
  detect: it is a property of the panel.

Two earlier attempts are worth naming so they are not re-proposed: a
signed-distance field, which cannot hold a one-pixel stem at an even weight or a
corner at all, and a hand-written scanline rasteriser with a vertical grid fit,
which came close enough to argue about and still read as a bitmap font.

### The blob

The atlases and the cell table are coded by `tools/font_codec.h` — one
definition shared by the tool that writes them, the tool that checks them and
the two files that read them at run time, because a format with two
implementations has two chances to be a different format. It is **LZ77 matches
and literals through an adaptive binary range coder**, in the LZMA style: a
match finder over a 64 KiB window, and every bit — match or literal, repeat
offset or new, each bit of a length, an offset or a byte — coded against one
adaptive probability, with a literal's chosen by the byte before it. A coverage
atlas is mostly nothing and what is left repeats: the same stem, bowl and serif
rasterised into hundreds of glyphs at one scale, and rows that repeat at one
distance. A run of zeros is a match at offset 1 with an overlapping copy, so
one format handles both.

Nothing may join the link line, so the coder is the project's own and small
enough to check by eye. What the alternatives make of the same 5,271,552 raw
atlas bytes — a measurement taken on an earlier, larger character set, which is
why its raw size is not the shipped one's:

| | coded | |
|---|---|---|
| the same matches packed into whole bytes (LZSS) | 1,424,799 | no entropy coding |
| zlib -9 | 1,340,097 | needs zlib |
| LZMA2 at the same 64 KiB window | 1,073,915 | needs liblzma |
| xz with a 1 MiB dictionary | 875,990 | needs liblzma |

An entropy-coded stage at an equal window is therefore worth about 350 KB and
a larger window about 200 KB more. The first is taken. The second is not: the
window is what the offset model and the encoder's working set are sized by.

What ships is 926,059 bytes of atlas for the six scales, each 1536 texels
wide, and the cell table beside it:

| scale | atlas | coded | decode |
|---|---|---|---|
| 1.0 | 1536×229 | 92,113 | 4.75 ms |
| 1.25 | 1536×342 | 131,180 | 6.76 ms |
| 4/3 | 1536×362 | 133,871 | 6.90 ms |
| 1.5 | 1536×473 | 163,533 | 8.46 ms |
| 5/3 | 1536×542 | 180,171 | 9.33 ms |
| 2.0 | 1536×766 | 225,191 | 11.79 ms |

**The decode time is the price, and it is several times what a byte-packed
codec costs.** Those figures are the decoder alone, standalone on this machine,
best of five at `gcc -O2`; packing the same matches into whole bytes decodes in
0.49 ms at 1.0 up to 1.59 ms at 2.0. A range coder pays for every bit it
produces, and the entropy stage buys about half a megabyte of every binary
that links the library with it. The trade is taken because the cost is paid
per atlas and only when the scale changes — one atlas is live at a time — and
a scale change already rebuilds the image and waits on a queue. With the blob
in it, the 0.5.0 `libfenestal.so` was 1,200,592 bytes on X11 and 1,267,328 on
Wayland, and `libfenestal.dll` 1,262,489.

It expands into a **scratch block, not into the mapping**, and that is the one
rule mapped memory has: never read back from it. `find_memory_type()` takes the
first `HOST_VISIBLE|HOST_COHERENT` type, which is uncached — write-combining,
whether it is across the bus or in system memory — so linear writes stream and
reads are single uncached transactions. This codec reads: a match is a copy
from what has just been written, and a run is a match at offset 1, so most of
the output comes back through those reads. Decoding in place runs the whole
expansion at uncached-read speed, and the cost scales with the atlas. Timed
inside `build_atlas()` on the X11 build, the median of five start-ups at each
`--scale`, with the decoder pointed at the mapped staging memory for the one
column and at a zeroed scratch block, then copied, for the other:

| scale | atlas | in the mapping | scratch + `memcpy` |
|---|---|---|---|
| 1.0 | 1536×229 | 19.9 ms | 5.5 ms |
| 1.25 | 1536×342 | 26.0 ms | 8.3 ms |
| 1.5 | 1536×473 | 33.1 ms | 9.7 ms |
| 2.0 | 1536×766 | 51.7 ms | 13.6 ms |

The scratch column is the standalone decode above with the allocation and the
copy on top. The mapping costs three and a half to four times that — less of
a multiple than a byte-packed decoder shows, whose own arithmetic is nearly
free, and more milliseconds.

Not only a startup cost. `text_set_scale()` comes back through `build_atlas()`
on every scale change — a window dragged between monitors of different
density — and it runs on the render thread, behind a queue drain, with the
frame waiting on it: 52 ms of stall on a 2.0 display in place of 14.

Asking `find_memory_type()` for `HOST_CACHED` instead is the other way round and
the wrong one: the type is not guaranteed to exist, it is the slower side for
the device's own read during the copy, and the same helper serves the instance
buffers, which are written and never read and want exactly the memory this one
does not.

The cell table goes through the same codec. It is a `GlyphCell` per (scale,
size, glyph), and most of it is the absent marker, because a size is baked for
its own set of code points rather than for the whole index space. As a braced
initialiser it is 399 KB of `.rodata` no compressor ever sees; coded it is 28
KB. `text_load_cells()` in `src/core/text_measure.c` decodes it once at startup,
before the window exists — the decoration layout measures the title while
handling the first configure.

### Three tables decide the cost

`SCALES` is every scale mutter's ladder reaches up to 2.0 — 1, 5/4, 4/3, 3/2,
5/3, 2 — so every scale a compositor is likely to name in that range is exact.
Nothing above 2.0 is baked.

`FACE[]` says which typeface each size is cut from. `TEXT_BODY` and `TEXT_LARGE`
are the monospaced face: everything the overlay composes is tabular, and
monospace is what makes columns line up. `TEXT_UI` is Ubuntu Sans, for chrome
that is prose rather than data — the titlebar title and the words of the GPU
calibration's panel (`app_rank_draw()`).

Both faces share one atlas per scale, because one atlas is uploaded at a time
and text in either has to be drawable in the same frame. The sans face's cmap
turned out to be a strict subset of the mono's, so the code point index covers
both.

`SIZE_RANGES` is what each size is baked **for**, as inclusive code point
ranges, and it is where the megabytes are. Each size's share below is
measured the same way: the shipped blob, less the blob `mkfont` bakes with
that size cut down to U+FFFD alone. The three shares sum to 3% over the whole,
because a coded stream is not a sum of its parts.

* `TEXT_LARGE` is baked for one string's worth of characters — a space, the
  digits, and `F`, `P`, `S` — and nothing else. It is 1.5x the cap height and so
  2.25x the atlas area per glyph: baked for everything `TEXT_BODY` carries, it
  takes the atlases from 4.17 MB raw to 9.04 and the shipped blob from 926,059
  bytes to 1,809,589 — 0.88 MB to serve those fourteen. As it is, the size
  costs 26,192 bytes. A caller drawing anything else at this size has to widen
  the set, and gets `���` until it does.
* `TEXT_BODY` draws strings nothing here authors — an output's name comes from
  the compositor, a GPU's from the driver — so it carries every script one of
  those could plausibly be written in. What it leaves out is left out because it
  cannot appear in an EDID string or a Vulkan device name: Latin Ext-B, Greek
  Extended, and the decorative symbols. It is 594,757 bytes of the shipped
  blob, a little under two thirds.
* `TEXT_UI` is baked for U+0000–017F, U+2000–218F and U+FFFD only — Latin,
  punctuation and the replacement character — and **borrows the rest**. Beside
  the calibration panel's own English, the one string the library sets in it
  is the window title, which is `FnlInitDesc::title` and therefore a caller's,
  in whatever script that caller writes. A code point `TEXT_UI` lacks and
  `TEXT_BODY` has is drawn, and measured, with `TEXT_BODY`'s cell:
  `FONT_SIZE_FALLBACK`, applied in `cell_for()` in `src/core/text_measure.c`,
  so the pen and the measure cannot take different cells. A Cyrillic or Greek
  title therefore comes out in the mono face rather than as a row of
  replacement characters — which is what narrowing the set costs without a
  fallback. Baking the whole of `TEXT_BODY`'s set a second time in the sans
  face avoids both and costs 253,315 bytes more of blob; what `TEXT_UI`
  carries as it is costs 334,445. Borrowing is legitimate only because the two
  sizes share a cell, a cap height and a line, so the borrowed glyph sits on
  the borrower's baseline at its height; `verifyfont` checks that, and that a
  fallback is one step and never a chain.

`U+FFFD` is in every set deliberately. A code point neither a size nor its
fallback is baked for is marked absent in the cell table and `cell_for()` hands
back the replacement character's cell, so drawing something new at `TEXT_LARGE`
without extending the set comes out as a row of `���` — visible at a glance
rather than silently blank. `verifyfont`
checks that every absence is exactly where the set says it should be, and that
**U+FFFD has ink at every size**: Ubuntu Sans carries no U+FFFD at all, so what
got baked for `TEXT_UI` is FreeType's own missing-glyph box, and in a face where
that box is blank every substitution would draw nothing.

### Measuring

The layout takes its grid from the typeface — `text_advance()`,
`text_height()` and the line step, `FONT_LINE_PT` — rather than from a step
chosen by the overlay. All three are
derived at the bake and land in a small generated `font_metrics.h` that `app.h`
includes, so the decoration layout gets constants folded in rather than calls
into the text code; the bulky tables stay in `font_data.h`, which only the
files that read them include — `src/core/overlay.c`,
`src/core/text_measure.c` and each renderer's atlas upload
(`src/renderer/vulkan/text.c`, and the others' `overlay.c` or `.m`).

A proportional face has no per-size advance, so its cells carry one each and
`text_advance()` answers 0 for `TEXT_UI` rather than a plausible-looking number.
`text_width()` resolves each code point through the same `cell_for()` the pen
does — one substitution path, not two, the fallback size included — so the
measure and the drawing cannot disagree about where a proportional string ends.

`text_width()` returns the width to **reserve**: the widest any baked scale
would draw the string, converted back to points. Taking the advances at 1.0 is
wrong, because the pen walks the advances baked at the scale in force and each
of those is one glyph's advance rounded to a whole pixel — a sum of rounded
advances is not the rounded sum, and it runs *long* at every scale. Reserving
the widest costs a few points of slack at the other scales, which is the right
way round: a layout figure too small is a collision, one too large is a gap.

The title's width is kept as `App::title_w`, an atomic, because both threads
ask for it — the hit test on the input thread and `draw_decorations()` on the
render one — and `text_width()` walks every baked scale rather than the one in
force. It is the width of the title the frame *latched*, not of the one last
set: taken in `fnl_init()`, before either thread exists, and again by the
render thread whenever the latched title changes (`draw_state_update()`).
`fnl_set_title()` does not measure. Measured there, the width ran a frame
ahead of the string drawn, and a shorter title set from `on_record` drew the
longer old one through the controls laid out for the new.

There is deliberately **no measure in pixels**, and the obvious implementation
of one is wrong. Nothing needs it: every string the overlay positions by
measurement is monospaced, and the title is drawn at a fixed margin and only
ever asked whether it *fits*. If content ever does want one, the monospaced case
is the trap — the pen rounds each point position on its own, deliberately, so
summing the cells' advances is a different distance and drifts by 8 px over a
25-character string at 4/3 and 5/3. Convert the point-space width once instead,
which is the distance the pen is approximating. The proportional case is the
other way round: there the cells *are* what the pen steps by, so summing them is
exact.

### Licence

`fonts/ubuntu/` holds Ubuntu Sans 1.006 and Ubuntu Sans Mono 1.100, from
Canonical's own releases. They are under the Ubuntu Font Licence 1.0, which is
neither OSI-approved nor DFSG-free and which requires derivatives — including a
subset baked into a glyph atlas — to be renamed. Condition 1 requires the
licence and the notice to travel with every copy of the font software, which the
baked atlas is a modified version of, so both are assembled into `.rodata`
beside the atlas, printed by `--licence`, and reachable through
`fnl_font_notice()` and `fnl_font_licence()`. Once this is a library, "every
copy" means every binary somebody else ships, and a host that cannot get at the
text cannot satisfy a condition it has inherited.

The atlas is named in that notice: **Fenestal Glyphs Sans** and **Fenestal
Glyphs Mono**. Condition 2 renames a modified version by how far it departs --
(b) one "Substantially Changed" takes a name clear of the original's, (c) one
that is not keeps it with "derivative X" appended -- and a fixed-size coverage
subset with no outlines is easily told from the faces it came from, so (b) is
the condition it meets. The atlas carries no name field of its own, which made
"nothing to rename" a possible reading; a name stated in the notice that
travels with every copy settles the question on any reading, where that one
would have rested on an argument, and the licence ends for a broken condition.
"Baked from Ubuntu Sans" stays, as acknowledgement, which condition 3 allows;
nothing calls the atlas by either face's name.

The tree's own terms are `LICENSE`'s -- MIT for the code, CC BY 4.0 for the
documentation, CC0 for `demos/` and `examples/`, which are written to be copied
out -- and `THIRD_PARTY_NOTICES.md` lists what is in it and not its own, the
font first.

## Backend notes

### Wayland

Protocol stubs are generated from the vendored XML in `protocols/`: xdg-shell,
xdg-decoration, cursor-shape, viewporter, fractional-scale, pointer-constraints,
relative-pointer, presentation-time and text-input. No `wayland-protocols`
package is needed.

The native Wayland window **cannot be captured** by an ordinary client: GNOME
answers `org.gnome.Shell.Screenshot` with `AccessDenied`, and X11 grabbers
cannot see a Wayland surface at all. Two ways round it, and they answer
different questions — the Mesa screenshot layer takes the image out of the
swapchain, which is what the client *produced*, and the Screenshot portal shows
what the compositor *made of it*. See `tools/drive/README.md`, *Capturing a
frame* and *Driving a window*.

**GNOME advertises every global the backend binds**, so on this desk the
branches for a missing one have never run: `create_window()` refuses a
compositor without `wl_compositor` or `xdg_wm_base` and treats every other
global as optional, `wl_seat` too (no seat, no input). weston is where they
run — `wayland-vulkan-bare`, on the bare machine's VT with no panel or shell
client around it (`tools/drive/bare_driver.py`). The backend says once, at
open, which optional globals were not bound, and is silent where every one
was. weston 14.0.2 on the thinkpad leaves four:

    not bound, so their fallbacks are in use: cursor-shape, xdg-decoration,
    fractional-scale, text-input-v3

So there the arrow is the compositor's own, decorations are client-side by
xdg-shell's default rather than by asking, the scale is the outputs'
integer one, and text is typed through the library's composer. The round
is 42 checks with none failed and seven not applicable, and 45 with
`--capture` on one output -- 49 across two, below -- where
`weston-screenshooter` (allowed under `--debug`) shows the titlebar, its
absence in fullscreen and its return; `asan`, `tsan` and `gc` are 42
each, none failed, under the validation layer.

A 1280x720 window does not fit a 1366x768 panel with room around it:
weston opened it about ninety pixels down, its bottom edge below the
screen, and the first round's press aimed at the bottom grab ring was
clamped onto the scene and captured the pointer. The round therefore takes
the window in as it does on X11, by a harness that finds the window
without a server to ask (`make_room()`): the pointer, unaccelerated, is
pushed into the corner of the top-left output, and a point reached from
there is a known place the echo names in window points. The window is
moved by its titlebar, taken in by the grab ring's top-left corner to
1066x514, and centred. It is gripped by the title and not the middle of
the bar: at 1066 points the middle is the FSAA checkbox, the controls
being right-aligned and of fixed width, and a press there toggles it and
moves nothing.

**Across two outputs.** weston places each output right of the last, in
the order it enabled them -- the kernel's connector order -- and nothing
a client is told says where; its scene graph does, under `--debug`
(`weston-debug scene-graph`, read by `weston_layout()`): on the thinkpad
LVDS-1 at 0,0 and HDMI-A-1 at 1366,0. Every output is on, and the round's
is the one the pointer is parked on before the program starts, since
weston opens a window on the pointer's output. The corner takes more
than one push: a motion that would end where no output is leaves the
pointer on the output it began on, clamped, so from the second output a
push up and left stops at that output's corner. The harness goes up to
the shared top edge and then left in steps narrower than any output.
The move to the other monitor and back then runs as on GNOME: the
output's name and size follow the window there and back, told by
`FNL_EVENT_OUTPUT`. 49 checks with `--capture` on the panel, 48 with
`FNL_DRIVE_BARE_OUTPUT=HDMI-A-1`, none failed either way.

**Wine on Wayland opens on its primary, which no pointer moves.** wine
10.0's Wayland driver sorts the outputs with the one at (0,0) first and
the rest left to right, and makes the first its primary and DISPLAY1
(`output_info_cmp_primary_x_y()`, `dlls/winewayland.drv/display.c`). On
this desktop mutter has DVI-D-1 at (0,159) and DP-1 at (1920,0), so with
nothing at (0,0) the leftmost, DVI-D-1, is wine's first and its primary
both, where GNOME's primary is DP-1. `win32_open()` makes its window
with `CW_USEDEFAULT`, which wine puts on the primary wherever the
pointer is (seen from inside under both of wine's drivers, 2026-10-31),
so the round's pin by the pointer cannot reach another output there
(`PIN_UNHELD`); DP-1 at (0,0) in GNOME's layout would make it wine's
primary.

**A suspend needs nothing here either, on lavapipe.** `fenestal-pace
--draw` on weston, started detached as on X11, and the thinkpad put to
sleep for a minute (`rtcwake -m mem -s 60`, deep, the kernel's entry and
exit logged): its frame lines read one half-second window at 15 fps
across the sleep -- the clock is `CLOCK_MONOTONIC`, which stops while
the machine does, so the minute itself is not in any window -- and 60 fps
on either side, with no rebuild, nothing logged, and weston's picture
after it the window whole. weston restores its outputs and goes on
sending frame callbacks; the library is told nothing and needs nothing.
A device that can be lost is still owed (`TODO.md`).

**On schenker weston scans the window out, and a client drawing flat out
waits for it.** `fenestal-pace --draw --no-vsync`, release, 1280x720 on
the panel's weston, frames a second:

| | schenker | aspire |
|---|---|---|
| weston, Intel GPU alone | 3,798 | -- |
| weston, NVIDIA (GTX 1050 Ti / 960M) | 181 to 304 | 1,281 to 1,318 |
| the same, weston forced to compose (`WESTON_FORCE_RENDERER=1`) | 766 to 778 | -- |
| Xorg, NVIDIA, the same hand-over to Intel | 1,426 | 1,643 |

The software is the same on both: NVIDIA's own driver, 580.178.04, weston
15.0.1 with the same `weston.ini`, one 1920x1080 output, kernel 7.2.11
and Mesa 26.2.5 from the live key. Both hand over the same buffers by
linux-dmabuf (`WAYLAND_DEBUG=client`): linear, `AR24`, 1292x766, stride
5376. A schenker frame spent 3.0 ms in the acquire, 2.3 in the present
call and 0.01 on the device's fences; aspire's 0.08, 0.65 and 0.00 (the
frame line's `fence` and `acquire`, *Where the time actually goes*). The
GPU is never behind: the frame waits for its swapchain to have an image
back.

What differs is weston's plan for the window. On both it tries the
NVIDIA buffer on overlay plane 44 (`weston-debug drm-backend`). On aspire
the kernel's atomic test refuses it, so weston composes; on schenker the
test passes and the client's buffer is scanned out as it stands -- the
presentation feedback's flags read 15, `ZERO_COPY` among them, where
aspire's read 7. A buffer on the plane is held until the flip that
replaces it: one buffer a refresh came back 20 to 30 ms after its commit
on schenker (15 to 20 on aspire, from weston's composition), the rest
within 0.5 ms, and NVIDIA's swapchain, at five images there to aspire's
three, ran dry each refresh and waited; the commits came in bursts with
gaps of 10 ms and more. Forced to compose, schenker's acquire fell to
0.15 ms and the rate rose fourfold. The remaining gap to aspire is the
present call, 1.0 ms against 0.65, which is NVIDIA's own and was left
there. So a plane weston offers -- worth having at the refresh, a copy
saved -- costs a client drawing flat out; at 60 Hz it costs nothing. The
display decides, not the library or weston's configuration: Kaby Lake's
accepts the overlay and Skylake's does not.

Two things it was not. schenker's CPU sat near 900 MHz through it: HWP
reading a thread that mostly waits, a consequence -- no PROCHOT, power or
thermal limit set or logged (`MSR_CORE_PERF_LIMIT_REASONS`, the thermal
status registers), both machines ask HWP alike, and pinned to 3.1 GHz the
rate went only from 303 to 361. And the GTX 1050 Ti was 30 to 37% busy,
stepping down from P0 to P5, where the 960M ran at 87%: little work, not
too much.

**The geometry and the scale ride the present, beside the ack.** Both are
double-buffered wl_surface state, applied by whichever commit comes next,
and the next commit is not always this frame's. They were sent at the top
of the turn, ahead of the acquire and the recording, and the ack only
before the present; on schenker's Intel GPU under weston 15 a restore
from maximized lost the connection with

    xdg_surface geometry (1280 x 754) does not match the configured
    maximized state (1920 x 1080)

-- the restored geometry had reached the commit of a frame still answering
the maximized configure. lavapipe never drew fast enough to put a commit in
the gap. `app_surface_sync()` is now called by the renderer immediately
before the present, after which only that present's commit can carry
them; three rounds on that device and weston, none failed.

**And the ack goes first when the window leaves for floating.** The same
error came back on the thinkpad under ThreadSanitizer, lavapipe on two
threads: the weston round lost the window at the restore in four runs of
seven, with

    xdg_surface geometry (1066 x 548) does not match the configured
    maximized state (1366 x 768)

`WAYLAND_DEBUG=client` showed no commit between the restored geometry and
the error. Mesa's present had committed twice the frame before -- the
buffer with `wp_commit_timer_v1.set_timestamp` and a FIFO barrier, then an
empty commit waiting on it -- and weston held the empty one back to the
next vblank. weston 15 keeps no xdg_surface state in a held content
update: `set_window_geometry` waits in `next_geometry` for whichever update
is applied next, `ack_configure` sets the acked state the moment it
arrives (`libweston/desktop/xdg-shell.c`), and the maximized check runs as
an update is applied. The vblank fell after the geometry and before the
ack had arrived -- `app_surface_sync()` also flushed between the two -- so
the update held back from an older frame was applied with the new
geometry against the maximized state.

Nothing in this library can see a held update coming, so the two requests
are ordered so that weston may apply one without the other: the ack first
when the new state is floating (any geometry goes), the geometry first
when entering maximized or fullscreen from floating (the old state allows
the new size), the ack first from maximized into fullscreen and the
geometry first back again (fullscreen only bounds the size). The flush
comes after both. Eight runs of that round on the thinkpad, none failed.

**OpenGL draws here through EGL and a `wl_egl_window`** (`wayland-gl`,
`src/renderer/gl/egl_wayland.c`), which differs from the X11 bridge in three
ways, each seen working on this desk (radeonsi, Mesa 26.0.8, GNOME):

* **The swap never waits.** Under an interval of 1 Mesa's
  `eglSwapBuffers` waits for a frame callback of its own, with no timeout,
  and a hidden window is sent none, so the render thread would stop in the
  swap until the window came back. The surface is given 0 whatever vsync
  asks, and vsync is the library's own frame callback, which times out
  (`FRAME_WAIT_S`) -- the throttle Vulkan has here too (*Late latching*).
  The status line says `frame callback (vsync)`, and with vsync off,
  where nothing paces, `swap interval 0`. Minimized with the scene
  clock running, the window went on committing until the off-screen skip
  stopped it, and a `SIGTERM` six seconds later exited cleanly in 0.02 s:
  nothing waited in the swap.
* **The native window has a size of its own.** While the size is changing
  it is `app_buffer_room()`'s, and at rest the window's; an unchanged size
  is not handed to `wl_egl_window_resize()` again, because Mesa allocates
  the next buffer for a size it is handed. The viewport's source shows the
  top-left extent. What the room saves is unmeasured.
* **The configuration has alpha**, for the transparent grab ring, which is
  Wayland's pre-multiplied ARGB as Vulkan's composite alpha is; with no ring
  (fullscreen) the EGL surface is made again with `EGL_PRESENT_OPAQUE_EXT`
  over the same native window, so the compositor may scan it out (*Composite
  alpha*). A driver without `EGL_EXT_present_opaque` keeps the alpha
  surface throughout. Read off `WAYLAND_DEBUG=client`: the buffers are
  `AR24` windowed, `XR24` in fullscreen and `AR24` again after it.

The stress round is 57 checks with none failed and seven not applicable,
and 60 with `--capture`, where the compositor's picture shows the titlebar,
its absence in fullscreen and its return. Under `asan`, `tsan` and `gc` it
is 60 each with `--capture`, none failed, and `minimal.py` 20 each; the
`analyzer` and `clang` trees build with no warning. ThreadSanitizer needs
radeonsi's debug info for two entries of `tsan.supp`, which name the
driver's own hand-offs between its threads. A Debug tree's GL debug output
reported nothing, and is shown to be on by `FNL_SELFTEST=gl`
(`selftest.py`), which it reports.

The bridge is the one file outside `src/backend/wayland/` that names
Wayland: EGL takes no `wl_surface`, only a `wl_egl_window`, and only
libwayland-egl's calls make one, so `check-layering.py` excuses it by name.

### X11: xcb, not Xlib

The render thread is the reason. Mesa's WSI blocks in a connection once a frame
to present, so a connection is live on that thread whatever this library does.
It gets one of its own (see *Frame pacing and the off-screen skip*) — which
settles where the events go, not the thread-safety: the two connections are
still opened, flushed and torn down across threads. libxcb is thread-safe by
construction. Xlib is thread-safe only after `XInitThreads()`, and a library
that must call that before its host's first Xlib call is a library that has to
be initialised before anything else in the process.

It is also the extension Mesa implements natively: `VK_KHR_xcb_surface`, with
the Xlib one a wrapper that fetches the same xcb connection back out of an
Xlib `Display`.

**Core protocol only.** Every extension library — `xcb-randr`, `xcb-xkb`,
`xcb-icccm`, `xcb-cursor` — would be one more entry on somebody else's link
line. Two things follow, and both are paid rather than avoided.

*The extension requests are hand-encoded* against `xcbext.h`, which is the
interface the generated bindings are themselves written against
(`x11_ext.c`): RandR's `QueryVersion`, `SelectInput`, `GetMonitors`,
`GetScreenResourcesCurrent` and `GetCrtcInfo`; XKB's `UseExtension`,
`SelectEvents`, `GetNames` and `PerClientFlags`; SYNC's counter, for
`_NET_WM_SYNC_REQUEST`; and Present's `QueryVersion` and `SelectInput`,
with the Generic Event Extension's version its events need.
What makes that safe is that every one is *asked* and not assumed — a server
without the extension answers nothing, a server that dislikes the encoding
answers an error and no reply, and both come back as a NULL that switches the
feature off and leaves a documented fallback in place. The worst a mis-encoded
request can do is cost what it was for: the monitor's name, the keyboard's
detectable auto-repeat, the manager's wait for a frame, a shown stamp. Two
ICCCM structures, `WM_NORMAL_HINTS` and `WM_HINTS`, are laid
out by hand for the same reason, and they are arrays of `CARD32` in the
specification.

*The cursors come from the core cursor font.* Themed X cursors live in Xcursor
files that `libXcursor` or `xcb-cursor` reads. The shapes this window ever
shows — the arrow, the eight resize edges and the eight more a caller can ask
for — are cut from the font the server has carried since 1987, so the pointer
over an edge or a caller's text field is the right shape in the wrong theme.
That is the one visible difference from the other backends, and it is what
bounds `FnlCursor`: see *The pointer's shape*.

**The frame.** `_MOTIF_WM_HINTS` with its decorations field zeroed is the
borderless-window convention every window manager implements: it predates EWMH,
it is what GTK and Qt use for their own client-side decorations, and it removes
the manager's titlebar and border without removing the window from the taskbar,
the alt-tab list or the snap layout — which is what an override-redirect window
would have cost. The X11 spelling of `WM_NCCALCSIZE`, and it fails the same way
if the manager declines.

A press on a resize edge, or a drag on the caption past `DECO_DRAG_SLOP`, is
handed to the manager as `_NET_WM_MOVERESIZE`. That buys the manager's own drag,
snap and edge resistance, which is worth having and is not worth reimplementing.

Where there is no EWMH manager at all — `_NET_SUPPORTING_WM_CHECK` unset, which
is a bare X server — there is nobody to hand it to, and the backend runs the
move or resize itself, one `ConfigureWindow` per motion event; fullscreen and
maximize likewise become a rectangle this side computes. Unreachable on any
desktop worth running this on, and driven on one machine that is nothing else:
`x11-vulkan-bare` (tools/drive/README.md *And X11 with nobody managing it*), a
ThinkPad Edge with Debian 13, Xorg 21.1.16 on a 1366x768 panel and lavapipe.
The round there is 62 checks, none failed: the caption's drag, the
bottom-right edge, fullscreen and maximize in both orders and back, and the
keyboard focus, which with nobody to give it is the server's PointerRoot.
Maximize and fullscreen share one saved rectangle, the window with neither on,
so the geometry after either changes is whichever is still on and the
rectangle comes back only when neither is; restored as soon as one went off,
a maximized window left fullscreen at its restored size. Every edge and
corner has been dragged there, two steps in and two out: the edges not
dragged hold, the dragged ones follow the pointer, and the width moves in
the steps below.

With a second monitor -- a Samsung at 1920x1080 on its HDMI, right of the
panel -- the round is 65 checks on the panel and 64 on the Samsung, none
failed, and the move between them runs both ways: the window crosses, its
output and its room follow, and both come back. A monitor on a bare server is
only gone when somebody switches its output off, and nobody does. Pulled with
the window on it, the output reads `disconnected` and keeps its mode, its
place and its RandR monitor, so the library keeps naming it and the window
stays on a screen nobody can see; plugged back in, the picture returns with
nothing re-set. Switched off with `xrandr --output HDMI-1 --off`, as a desktop
would on a pull, the monitor leaves the list, and a window on it would be on
no screen at all with nobody to move it. So the backend does what a manager
would (`bring_on_screen()`, after each re-read of the list): a window that
touches no monitor goes onto the primary, centred in its room and no larger
than it, and a fullscreen or maximized one becomes that monitor's rectangle,
with the rectangle saved for leaving the state brought along too. On the
thinkpad, the testbed on the Samsung and the Samsung switched off: the window
from (1686, 163) to (43, 7) on the panel at 1280x754, one
`FNL_EVENT_OUTPUT` naming `LVDS1`, one log line; fullscreen, from the
Samsung's 1920x1080 to the panel's 1366x768, still fullscreen.

**Keys are named by XKB's key names**, read with one hand-encoded
`XkbGetNames` (`x11_key_names_ask()`), and not by the evdev code plus 8
alone. A server whose keycodes are not evdev's numbers the same keys
otherwise, and every XKB keymap names each key by where it sits -- `<AC01>`,
`<UP>` -- whatever its keycode or the layout. The names are re-read after
any XKB keyboard event, by the next key. With the thinkpad's server switched
to xfree86's keycodes under a running testbed (`setxkbmap -keycodes
xfree86`), twelve keys sent by their xfree86 codes -- Up at 98, Home at 97,
the right Control, Alt and keypad Enter among them -- were each named right,
where the subtraction would have named 98 the keypad's divide; switched
back, the same keys by evdev's codes, likewise. A server without XKB has no
names and keeps the subtraction.

**A suspend needs nothing here, on lavapipe.** The testbed drawing its moving
scene, started detached on the thinkpad, and the machine put to sleep for a
minute (`rtcwake -m mem -s 60`, deep): its frame lines read one window at 0
fps across the sleep, one at 14 and then 47-53 fps as before, with no
rebuild, no lost device and nothing logged, and the frame captured after is
whole. Nothing tells this backend of a resume and nothing had to. What it
cannot say is a GPU's: lavapipe has no device to lose. A program run inside
the harness's ssh session does not survive the sleep -- the link is reset
and takes the program with it -- so a sleep is tested on one started
detached.

**No invitation to pace on.** There is no `wl_surface.frame` and no waitable
swapchain object, and FIFO present does not survive a swapchain rebuild, so the
frame loop spaces its own turns here and on Win32: see *Frame pacing and the
off-screen skip*, which has what that is worth. The Present extension's
completions, which the backend selects, encoded by hand, for the shown stamp
and the sync hold (below), are not taken as one: `invites` is false here.

**lavapipe leaks a shared-memory segment per swapchain image, so run it with
`MESA_VK_WSI_DEBUG=noshm`.** Mesa's software swapchain hands each image to
the server through MIT-SHM -- `xcb_shm_attach`, a pixmap on the segment --
and its teardown (`x11_image_finish()`) frees the pixmap and its own mapping
but never sends `xcb_shm_detach`, so the server holds every segment until the
client disconnects. Counted on the thinkpad at 250 sizes a second: the
program's `shmat` and `shmdt` within nine of each other every second and its
swapchain creates and destroys within three, while the segments marked for
removal and still attached once climbed 380 a second, to the system's 4096
in about 1,360 rebuilds. Past that every attach fails, Mesa's presents fail
with them (BadAccess on `ShmAttach`, BadDrawable on Present), and a destroy
then waits on a Present event that never comes; the window stopped with the
render thread behind the reaper's lock. It is the GLX path's leak, fixed
there in 2019, and it is in Mesa 25.0.7, 25.2, 26.1 and 26.2 alike. Nothing
in this library can send the detach for it: the segment is the driver's. With
`noshm` Mesa sends each frame down the socket instead, nothing is attached,
the same run finishes, and on a panel and a CPU of 2010 it is the cheaper of
the two everywhere but at rest (see *Frame pacing*). It reaches only Mesa's
software path, so setting it where a GPU draws changes nothing. Left to the
caller and not set here: it is the process's environment, read by whatever
else in it loads Vulkan.

**And `noshm` tears unless the server is tear-free.** Each frame then
arrives as a `PutImage` into the window, which the server copies to the
screen when it comes and not at a refresh, so FIFO paces the frames and
says nothing about when they are seen. Xorg's `modesetting` driver has no
`TearFree`; the thinkpad runs the Intel driver instead, SNA with
`TearFree` on (`/etc/X11/xorg.conf.d/20-intel.conf` there), and the tearing
is gone. The copy to the shadow buffer is the GPU's: `pace.py` read within
a sweep's spread of the `modesetting` figures, on the panel and on the
Samsung at 1920x1080 alike (60 fps from 30 sizes a second up, 98 to 117% of
a core; the border row 42 to 44 fps at 264 to 272%), and the round is 66
checks, none failed. That driver names the outputs `LVDS1` and `HDMI1`.
It offers no DRI3, and Mesa uses MIT-SHM only beside DRI3, so under it the
leak above cannot happen and `noshm` changes nothing: every frame is a
`PutImage` either way. It stays the documented mode, since it is what a
server with DRI3 needs.

**A drag run here moves the width in steps of 16 pixels.** Widening the
window by its edge showed noise down the new strip on every step, a few
pixels wide and gone by the next frame. It is Mesa's: under `noshm` its
present (`x11_present_to_x11_sw()`, on a thread of Mesa's own) sends each
row at the image's row pitch, not its width, and lavapipe pads a row to 64
bytes. A frame drawn at the old width that lands in a window already wider
shows its padding, which is uninitialised memory, beside the server's
black. Counted in captures taken straight after each step, the noisy
columns were the old width's distance to the next multiple of 16, two of
them after 1086 and ten after 1254. A width of whole 16-pixel steps has no
padding, so `manual_moveresize()` rounds to it (`MANUAL_WIDTH_STEP`): the
same captures then show the old frame and black, and nothing else. Keeping
old frames out of a widened window instead would mean destroying the
swapchain ahead of every step, since nothing else says when Mesa's thread
has sent one. The rounding is on every device, because the backend does not
know which one draws; a size the caller asks for is never rounded.

**And the strip a step uncovers is the bar carried on.** Until a frame of
the new size arrives -- one lavapipe frame, tens of milliseconds -- the
server shows the window's background there. Black alone made a slab across
the titlebar on every widening step. The background is a tile one pixel
wide (`background_update()`): the library's bar colour for the bar's
height, black below, remade when the bar, the focus or a window taller than
the tile asks. Read straight off the server after each of eight steps
outward on the right and eight on the bottom, the strip was bar colour for
exactly the bar's 34 rows and black below, and nothing else. A bar the
caller draws, or none, is black throughout. No frame can be there sooner:
an X11 surface's extent is the window's, so the frame for a size cannot
exist before the window has it.

**NVIDIA's swapchain can stick out of date, and a new size frees it.** On
schenker's GTX 1050 Ti with NVIDIA's 580.178.04, presenting through PRIME
onto the Intel GPU's screen on Xorg with no manager, 7 of 13 stress rounds
lost the window in the eight drag-resizes. The render loop rebuilt for
good: 21 swapchains in 0.4 s, each made at 1392x860, the size
`currentExtent` and the server both gave, and each acquire on each one
`VK_ERROR_OUT_OF_DATE_KHR` with the window holding still. No frame meant
no events, so the round found no window. `tools/probe/wsidrag.c` is the
rebuild with Vulkan and xcb alone, and it sticks the same way, 1 to 4 times
in 20 rounds of the same drags, once it rebuilds at each ConfigureNotify's
size as the library does; 60 rounds that took `currentExtent` and rebuilt
only when told out of date never stuck. Once stuck, nothing on the Vulkan
side frees it: the device idle, the retired swapchains destroyed, no
`oldSwapchain`, a new `VkSurfaceKHR`, half a second with none alive, or a
new X connection. A configure at the size the window has does not either.
The window a pixel wider freed it at the first rebuild, 7 of 7, and a
pixel wider and straight back, both sent at once, 4 of 4. So a run of
`SWAP_STALE_RUN` (8) swapchains out of date at their first acquire, each
at the surface's own extent, has the pump do that (`swap_stale_note()`,
`x11_nudge_size()`); the longest run that ended by itself over the
probe's rounds was three. The window ends at its own size, and a caller
sees at most one resize there and back. Not under a manager holding the
window maximized or fullscreen, which a configure would ask to let go.
Intel's GPU on the same probe never stuck, nor NVK on the same Xorg, nor
NVIDIA's driver on weston, so the other backends' entries do nothing.
`FNL_FAULT=stale-swapchain%300` forces it.

Sizes asked faster than that costs, it costs the frame rate. aspire's GTX
960M on the same driver, `pace.py` under bare X11: 50, 7 and 20 fps at
45, 60 and 250 sizes a second, where its Intel GPU and NVK on the same
server held 60 at each, and weston on the same driver 60. At sixty a
second the loop turns at the display's rate and presents 10 to 20 times
a second: each turn rebuilds (11 ms), most new swapchains are out of
date at their first acquire, and a run of eight -- a nudge -- comes about
every five seconds. What answers it is below.

It is what `pace.py`'s bare columns have measured since the laptops'
keys boot NVIDIA's own driver (the boot images' repository,
2026-10-18), the library's choice of device landing on it:
the border row read 60 fps on aspire and schenker that day, on Intel's
GPU or NVK, and 32 to 44 on every sweep after, the 60 row 6 to 31. Not
the library: the same build, 2026-10-29, both machines, Intel's GPU and
then NVIDIA's -- 60 sizes a second 61 and 59 fps against 37 and 11, the
border row 59 and 57 against
41 and 32. A bare column says its device (the ledger's `device`) and is
read with it.

**So with no manager the window takes one size at a time.** That backend
sizes its own window -- a step of a drag, a caller's size, fullscreen --
and nothing waited for a frame at the last size before the next. Measured
alone first (`tools/probe/wsidrag.c`, `WSIDRAG_RATE=60`), sizes that
reached the screen with a frame of their own, a second, on aspire's and
schenker's NVIDIA GPUs: 24 and 35 rebuilding at each configure, as the
library did; 44 and 56 rebuilding only when an acquire or a present says
out of date, at `currentExtent`, the alternative `TODO.md` named; 56 and
53 holding each size until a frame at the last had been presented, and
Intel's GPU 59 and 57 so, against 59. Held, then: `size_give()` keeps
the latest geometry asked for until the frame last presented is at the
size last given -- the render thread's flush records the extent and wakes
the pump -- or `SIZE_HOLD_S` (0.25) has gone by, for a frame that is not
owed; a move at the window's own size goes at once. Win32's rule
(`size_await()`), for the reason it has, and the drawing at the size the
program was told is kept, which rebuilding at `currentExtent` would not.
A manager holds the window to its sync request instead
(`_NET_WM_SYNC_REQUEST`, *X11: xcb, not Xlib*), so it is the unmanaged
window's alone. In the library, `pace.py` under bare Xorg, NVIDIA's own driver:
schenker 51, 60 and 60 fps at 45, 60 and 250 sizes a second, where it
was 49, 37 and 0; aspire 53, 56 and 60, where 33, 11 and 20; the border
row 58 and 49, where 41 and 32. Intel's GPU there is 60 at each rate and
59 and 57 in the border row, as before. Both machines' `x11-vulkan-bare`
and `x11-gl-bare` pass the round, 91 checks each (2026-10-29).

**On lavapipe a frame is its pixels, and the shader's arithmetic per
pixel.** The thinkpad, the testbed's scene moving at 1280x720: 37 fps on
lavapipe's four threads at 366% of a core, 15 fps on one
(`LP_NUM_THREADS=1`), about 65 ms of CPU a frame however it is split. Two
threads to four is the CPU's hyperthreads and buys 9 fps for 80% more of
it. Smaller windows follow the area, some 2 ms a frame and 26 ns a pixel;
the help panel's text is 2.5 ms of it, and the X server's copy 13% of a
core. Most of it was `scene.frag` linearising the colour its branches had
merged into, three `pow()` a pixel. Each colour is now linearised where it
is chosen, which leaves the driver's compiler constants to fold and the two
grey ramps one `pow()` each: 56 fps at 346%, 26 on one thread, 38 ms of
CPU a frame. The picture is the one it was: the still frame through
Mesa's screenshot layer, before and after, on lavapipe at 1280x754, is
the same bytes on an `_SRGB` surface, and on a `_UNORM` one
(`FNL_FAULT=unorm-surface`) 14,592 pixels of the colour bars differ by
one step of 255, the constants now rounded where they are folded. A
caller's scene on a software device pays for its arithmetic the same
way; the library's own share, the overlay and the copy, is a few
milliseconds of it.

**And the render scale does not pay there.** The scene pinned at 640x480 in
that window and stretched back over it, a third of the pixels, drew 40 fps
against 37 native before the shader change and 45 against 56 after:
`blit.frag` is one fetch, but lavapipe decodes four `_SRGB` texels for each
bilinear tap and encodes the result, at every window pixel, and that costs
more than the scene it saves. The stretch alone, 640x480 onto 1280x720 on
four threads, the median of 60 submit-and-waits in a program of its own
(`tools/probe/stretch.c`):
15.8 ms drawn as `blit.frag` draws it, 15.4 ms as `vkCmdBlitImage` with
`VK_FILTER_LINEAR`, and 7.1 and 6.2 ms in `_UNORM`; a clear of the same
image is 0.6 ms. So the blit is lavapipe's draw by another name, and the
cost is the sRGB conversion, half of it. Filtering the encoded values would
save that and is the mistake *Antialiasing* keeps the resolve from making,
so the render scale stays as it is, and on a software device it is not
the lever it is on a GPU.

**Opening costs three waits on the server.** Every request whose inputs are
known is sent before the first reply to any of them is taken, since a reply
taken early is a round trip of its own, and the calls are split into ask/take
pairs to make that possible (`x11_ext_ask()` / `x11_ext_take()`,
`x11_keymap_ask()` / `x11_keymap_take()`, `x11_work_area_ask()` /
`x11_work_area_take()`). One wave is the atoms and the two extensions' data;
one is every XKB and RandR request, the root's properties and the keyboard
mapping together; and the third is the monitors' names, which are atoms that
cannot be asked for until the monitor list has arrived. Two things are shaped
by it: RandR's events are selected on the root window, which is known before
there is a window of this client's, and `_NET_WORKAREA` is read for a fixed
number of desktops, so its length does not have to be asked first.

**The pump spends once a turn what events ask for many times.** A RandR
notification or a `_NET_WORKAREA` change only marks the monitor list or the
work area stale (`outputs_stale`, `work_area_stale`); the pump re-reads once at
the end of its turn, however many events asked. Each re-read is a blocking
round trip on the thread that pumps input, and a monitor change arrives as a
burst. `Xft.dpi` is likewise kept as it changes, so `update_scale` resolves
from the cached figure and asks the server nothing.

**The pump flushes; handlers do not.** Every request made on the pump's thread
is queued and goes out before the wait and at the end of the turn, which is
one write per turn rather than one per handler. Two sites flush for
themselves, and each says why: `recentre_pointer()`, whose warp cannot sit
behind a burst of motion (see *Pointer capture*), and `update_scale`, which
`fnl_init()` calls ahead of the first pump turn.

**OpenGL draws here through EGL over core xcb** (`x11-gl`,
`src/renderer/gl/egl_x11.c`), on `BackendState::gfx` for the Vulkan bridge's
reason. The stress round is 78 checks with none failed, `--capture` or
not, and the same under `asan`, `tsan` and `gc`, with `minimal.py` 19 each;
the `analyzer` and `clang` trees build with no warning, and ThreadSanitizer
matches the same three radeonsi entries of `tsan.supp` as on Wayland and
nothing else.

**The frame is blitted at the extent's height**, into framebuffer 0, whose
rows count up from the bottom. On Wayland the rebuild sizes the
`wl_egl_window` before the frame; on X11 the server sizes the window and
Mesa takes the new size at the next draw, so a frame in flight when a
resize lands could go into a taller buffer and land at its bottom. A probe
found it nowhere: the portal pressing a corner and moving it twelve steps
a run, the X server's picture of the window taken after each step, three
runs -- every frame at the window's top-left, cropped or with a strip
where the window had just grown, none pushed down and none black. The
count is a script's, checked against captures made to show both.

**And it presents fewer frames than Vulkan while the size changes.** Two
pace sweeps on the RX 590 under XWayland, the second beside a fresh
`x11-vulkan` tree: idle, 5 to 15 sizes a second and `draw` match on frames,
but at 250 sizes a second GL shows 49 fps in both against Vulkan's 60, and
at 30 a second it read 60 and then 33, against Vulkan's 60. (The border
row's 51 and 40 are not figures: they were read from frame lines picked
by position, one a second of the drag, and where fewer came the window's
"at rest" lines after it read as 0 fps -- one such cell, 31, was 62 and a
0. The row now counts frames under the press; these are owed again,
`TODO.md`.) GL's CPU is lower at every rate but idle, where both
are 0.2%, and as much where the frames match as where they do not -- at 60
a second 8.3% against 14.8%, both at 60 fps -- so fewer frames are not what
it is.

The frames are lost in Mesa's swap. `fenestal-pace` at 250 sizes a second
puts 17-20 ms of each GL frame in `eglSwapBuffers` and almost none in the
loop's own pacing, where `x11-vulkan` puts 13.5 ms in the pacing and 0.02
ms in the present. With nothing resizing (`--draw`) GL's swap is 0.3 ms
and the loop paces it to 60; at 30 sizes a second the swap averages 9 ms;
and at 250 under `vblank_mode=0`, Mesa's override to an interval of 0, it
is 0.3 ms again and the frames are back to 60, paced by the loop. Under an
interval of 1, Mesa's DRI3 swap (`loader_dri3_swap_buffers_msc()`,
`src/gallium/frontends/dri/loader_dri3_helper.c` in 26.0.8) keeps one
present in flight: it waits for the previous one's `CompleteNotify`, then
targets the vblank after the one that completion reported. While the
window's size changes XWayland's completions come late, and each late one
is a whole vblank lost -- a frame of 17-20 ms. Vulkan escapes it because a
resize rebuilds its swapchain, which then has nothing outstanding; an
interval of 0 escapes it because Mesa then waits for nothing and presents
`ASYNC`, which on a server with no compositor tears. Nothing in the
library's half waits longer.

**So the swap waits for nothing where a compositor shows the frames.** The
X11 backend asks at open whether a compositing manager owns
`_NET_WM_CM_S<screen>` -- in the wait it already makes for
`_NET_SUPPORTING_WM_CHECK`, so opening is three waits still -- and
under one (mutter's, for XWayland) the GL surface is given an interval of
0 from the start and the loop spaces the frames at the output's refresh,
as on Wayland; the status line says `paced by the loop (vsync)`. With
nobody compositing the swap stays the throttle, but for a resize: for
`GL_RESIZE_HOLD` (0.25 s) after the window's image last changed size it is
given 0, and 1 again once the size holds still -- a drag there may tear,
and nothing at rest does (`gl_choose_interval()`). Measured on this desk,
`fenestal-pace` at 250 sizes a second spends 0.28 ms in the swap and holds
60 fps, against 17-20 ms and 49; the manager resizing the window at 60 Hz
for six seconds, `x11-gl` holds 60 throughout, as `x11-vulkan` does; and
the pace sweep's 250 row is 60 fps at 8.0% of a core against Vulkan's 60
at 14.5%. Composited or not is read once: a compositor that comes or goes
afterwards is not followed.

**And a manager waits for the frame at a new size: `_NET_WM_SYNC_REQUEST`.**
Without it mutter shows a new size at once with whatever the window last
drew -- stretched, or edged in black -- until the frame at the size lands,
which is what a resize by the border looked like on every X11 build, and
worst where frames land late. The backend advertises the protocol where
the server has SYNC, with a counter of its own (`x11_ext.c`, encoded by
hand; SyncInitialize's reply discarded unwaited). A request's value rides
the configure handshake the core already has for Wayland: published as the
size's serial once the ConfigureNotify after the request has come, or 50 ms
after it where none does (`SYNC_CONFIGURE_WAIT`: a size the window already
has), so a frame at the old size cannot answer it -- and on every publish
from then until the counter has reached it, since a publish without a
serial clears a pending one -- so the loop draws that size even at rest; acked
by the frame that answers it, which clears the pending serial as Win32's and
Wayland's acks do; and the counter set by the flush after that frame's present,
never before, so the manager cannot see it first. Two first versions were wrong,
and the round said so: one dropped the value when the request and its configure
fell in different turns, and mutter waited out its own timeout on every step of
a drag (a 160x120 drag grew the window 26x19); the other left the serial pending
after the ack, so every frame acked and waited again, the window never rested,
and under ThreadSanitizer the drags lost the window in two rounds of three.

**Vulkan's present has to have been shown, not only made.** Mesa's X11
swapchain hands every present to a thread of its own ("WSI swapchain
queue", `x11_manage_present_queue()` in 26.0.8's `wsi_common_x11.c`), so
`vkQueuePresentKHR` returns before the server has the frame and a counter
set after it still reached mutter first; `x11-vulkan` kept the artifact
when `x11-gl` had lost it. So the frame that acks a configure waits to be
shown (`VK_KHR_present_wait`, every present carrying a `VK_KHR_present_id`)
before the flush, bounded at 34 ms, where the backend says the ack needs
it (`FnlBackendVtbl::ack_when_shown`: X11's, and Win32's, whose
`size_await()` is the same wait for the same reason). The instance is 1.0,
where those features are asked through
`VK_KHR_get_physical_device_properties2`, enabled where the loader lists
it; without both, nothing waits. Measured in a round: 41 sync requests,
707 waits that saw their frame shown -- median 6.6 ms, 90th percentile
15.6, longest 31.6 -- and two that answered `VK_ERROR_DEVICE_LOST` on a
swapchain being replaced, which the next frame is not; the result is not
read, and the counter goes then. Both X11 builds pass the round under the
validation layer with sync and best-practice checks, and under
ThreadSanitizer three rounds of three.

**And the wait is a thread's, not the loop's.** Made on the render
thread, it held the loop until the frame was shown, which comes back some
4 ms after the refresh, and the frame after it was spaced a period from
the rebuild before that: a drag lost one refresh in five. `pace.py`'s
border row on this desk's `x11-vulkan` read 45 to 48 fps in six rounds,
against 58 and 59 with no wait at all -- and the artifact back. Two
cheaper changes were tried and left out, both at 48 to 50: no spacing on
the turn after a frame was shown, and no late-latch hold while rebuilds
arrive. So the frame that acks is handed to a thread of the renderer's
(the shown waiter, `shown_main()` in `vulkan/frame.c`), which waits for
it and then releases the ack itself (`FnlBackendVtbl::ack_release`); the
flush the render thread makes after the present keeps it meanwhile
(`App::ack_held`). The render thread waits for the waiter only before a
swapchain is retired or destroyed, which a wait must not name, and before
the next ack, so a value never goes out on another frame's word -- a drag
waits at neither, its next configure coming after the release. The waiter
is started at the first frame that acks and joined with the device; with
no thread to be had the wait is made in line, as it was. Measured in
three rounds of the border row: 59 and 60 fps, and 362 to 370 sizes in
the drag against 383 to 398 waited in line and 508 to 514 with no wait.
How fast the window follows a drag is the handshake's, about 30 sizes a
second, and stays so; what comes back is the frames between, drawn at
the rate again. `x11-vulkan` passes the round with it, under
ThreadSanitizer with a clean log, and both bare machines' Xorg do. These
border figures are `pace.py`'s old count; counted over the press
(`frames_between()`, 2026-10-31, two sweeps) `x11-vulkan` draws 52 to 57
fps through the drag, wine's Vulkan 53 to 58, and OpenGL on XWayland 36
to 40, Direct3D 12 under wine 35 to 37 on X11 and 18 to 23 on Wayland --
the last three read near 60 before, the CPU of every column unchanged.
Direct3D 12's wait is made the same way (*Measured on Windows itself*).

**And where the renderer holds no ack, the backend holds it until
Present says the frame was shown.** OpenGL has no shown waiter, and
nor has Vulkan without `present_wait` (`App::ack_held` false), so the
counter was set by the flush right after `eglSwapBuffers`, before the
server had the frame; mutter then sent the next size at once, and the
next frame waited for it. The `--frame-times` line's `other` (the frame
less its parts) named the gap: OpenGL's frames through a drag 23 to 28
ms with some 16.5 of parts. Now `x11_flush()` keeps the value owed for
the present just made until Present's CompleteNotify for that present's
index (`x11_sync_answered()`, from `x11_present_event()` on the main
thread), bounded at 34 ms (`SYNC_SHOWN_WAIT`, Vulkan's bound) by the
pump's timeout; a cancelled present lets it go at once, and a new sync
request supersedes a kept one. Only once the window has had one
completion (`present_live`): Mesa's software WSI under `noshm` presents
by PutImage and answers none, and a wait that runs out turns the hold
off for the window, with one line. `x11-sync-at-swap` (a Debug tree's
fault site) releases at the flush as before. Measured (2026-11-01, this
desk, RX 590, `pace.py --border`): `x11-gl` 48 and 49 fps through the
drag in two sweeps against 36 to 40 before, `x11-vulkan` beside it 54
and 59; under the press OpenGL's frames now take 16.7 ms with `other`
0.0 to 0.1, the rest of the gap a few windows where no size came (110
to 170 ms of `other`, which Vulkan's column has too). On the `gc` tree
under the fault site 39 fps, its frames 20 to 85 ms with `other` 3 to 68,
and 53 without it. The wait never ran out in those or in the `x11-gl`
rounds, release and `gc`, 105 checks each and none failed. It runs out in
`x11-vulkan`'s stress round (2026-11-02, release and `gc` alike): once, at
the first press of the border drag after the window has been sized by
other means, the frame there 40 to 53 ms; and never in two `pace.py
--border` sweeps beside it, 1,390 sizes at 60 fps on both renderers. Not
matched to the wrong frame, not late: completions are taken in order, and
`FNL_X11_PRESENT_LOG=1` (a line for each frame pushed, answered or taken
back, and each kept value) shows the queue holding about five frames that
are never answered once the first swapchains have been made again -- the
presenter's serial starting over at 1 each time -- so that every
completion after is taken for a frame some five presents older: 19.8 ms
after its push at the median before those rebuilds, 180 to 230 ms after
(2026-11-02, the stress round, 1,818 completions). The kept value's frame
is then answered five presents late and the 34 ms runs out; the shown
stamps (`fnl_frame_shown()`) and `app_frame_answered()` read the same
queue. The steady border drags resize in place and make no new swapchain,
which is why they never run out. So a completion is matched by its serial:
Mesa's X11 WSI numbers each swapchain's presents from 1, the backend
numbers its queued frames the same way, restarting where the renderer
says a chain began (`App::present_chain`, bumped at each swapchain made),
and a completion is the oldest queued frame of its number, the frames
queued ahead of it dropped as never sent; a presenter whose serial matches
none is taken in order as before. The same round with it (2026-11-02): no
wait run out, 106 checks and none failed; of 2,926 completions the
medians 14 to 33 ms after the push, the queue empty or one deep, and 8
frames dropped, at 7 swapchains made again. Read back as a caller reads
them (`fenestal-dev pace`'s `pace: shown` line, each `fnl_frame_shown()`
stamp joined with its `fnl_frame_samples()` row; 2026-11-02, this desk):
`x11-vulkan` at 15 sizes a second, 180 swapchains made, 566 of 566
presents stamped, the latch to the screen 21.0 ms at the median and 21.4
at p95; at 60, 718 swapchains, 723 stamped, 6.4 and 23.0 -- a refresh or
two, where the old match read five more. `x11-gl` at 60, 681 of 737, 2.8
ms. At 15 it was stamped almost never, 3 of 544, its match exact all the
same: XWayland completes each flip 1 to 2 ms after the present with a stamp
from the refresh before it. With vsync on such a frame goes up at the first
refresh after its present, so `x11_present_event()` stamps it there --
the period the server's own counter gives between two completions (held
to 10 to 500 Hz), or the output's mode where none has -- and with vsync
off, where it tears on at once, still stamps nothing. Measured again: 505
of 543 at 15, 15.2 ms at the median; 737 of 738 at 60, 13.4; `x11-vulkan`
576 of 577 at 15 and 718 of 718 at 60, 21.1 and 13.2 ms. On the bare
machines' Xorg, at 15 and 60 sizes a second (2026-11-02): Vulkan on the
thinkpad 552 of 553 and 706 of 724, 17.4 and 16.2 ms; on aspire's NVIDIA
GPU 539 of 539 and 704 of 706, 6.2 and 19.1; on the Pi 559 of 559 and 630
of 720, 19.1 and 22.9; OpenGL on aspire 540 of 540 and 739 of 739, 14.8
and 4.7 (the thinkpad's and the Pi's GPUs have no OpenGL 4.1). NVIDIA's
presenter numbers each swapchain's presents from 1 as Mesa's does: 828
completions matched by serial, none dropped. A stamp before its frame's
latch, seen in 4 runs of 9 on aspire at 60 (7.1 to 10.9 ms before), was a
frame NVIDIA showed whose present it called out of date: the completion
came on the main thread before `vkQueuePresentKHR` returned, the frame was
taken back as never presented, and the next frame drew under its index and
was stamped a second time, the first stamp then joined with the later
frame. Now the backend's `frame_cancel()` sees the present answered
already and says so (`App::present_answered_anyway`), and the renderer,
through `app_frame_take_back()`, counts it as presented; and a frame's row
(`fnl_frame_samples()`) takes the presents before it for its index and is
presented where that count moved, so a refused one is drawn and not
shown under its own index rather than the one before. Six runs after: no
stamp before its latch, none twice, three refused presents counted.
Wayland's `surface_frame_cancel()` answers the same where it can: a
feedback the compositor said was shown before the take-back is a refused
present shown, and is counted; one not yet answered is marked taken back
and stamps nothing, as before, since a take-back comes before the
compositor's repaint and cannot tell a commit that went from one that did
not. Neither WSI here makes the case: Mesa's refuses before it commits,
and NVIDIA's on aspire's weston refused no present in three runs at 15
and 60 sizes a second, every frame presented and stamped once.

**Under wine none of this reaches the window.** wine's X11 window is
wine's, and it advertises no `_NET_WM_SYNC_REQUEST` (`xprop`), so a
Win32 program cannot have the manager wait; both `win64` renderers still show
the new size before the frame. And wine's Vulkan surface on X11 goes black
when the window moves, until the next present: a moved testbed's capture
went from its frame (17.7% mean grey) to 0% and stayed there, so under
wine a move asks for one frame (`on_move()`), after which the same capture
keeps the frame -- for Vulkan under wine on X11 alone, the case measured.

The lower CPU is Vulkan's rebuild and not anything GL saves. A frame costs
the two about the same -- `--draw`, with no size changes, is 6.2% of a core
for `x11-vulkan` and 5.2% for `x11-gl`, counted per thread -- and the gap
opens with the sizes: from `--draw` to 60 sizes a second, both at 60 fps,
Vulkan goes from 8.2% to 14.8% and GL from 6.5% to 8.3%, about 1.1 ms of
CPU a rebuild against 0.3. The rebuild column of `--frame-times` at 60
sizes a second says the same: 3.1-3.4 ms on `x11-vulkan`, 0.7-0.8 on
`wayland-vulkan`, and 0.3-0.4 for GL on either. A Vulkan resize makes a
swapchain, which on X11 is *Frame pacing*'s 3.6 ms inside
`vkCreateSwapchainKHR` -- the server's side of new images, and two new WSI
threads in place of the old (`WSI swapchain q` and `e`, which a per-thread
count sees come and go) -- where a GL resize keeps its EGL surface and Mesa
reallocates the one back buffer the next frame draws into. On Wayland the
swapchain is cheap, which is why the two agree there. A sweep of
`wayland-gl` beside a fresh `wayland-vulkan` tree matches on frames at
every rate -- 60 fps at 250 sizes a second for both -- and on CPU to
within a point (9.3% against 10.0% there, 8.0% against 8.7% at 60).

**On the laptops, and where there is no 4.5.** `x11-gl-bare` and
`wayland-gl-bare` on aspire's HD 530 and schenker's HD 630, the Intel GPU
each -- GL takes no `FNL_GPU`, and EGL came up on Mesa's Intel driver
though glvnd lists NVIDIA's vendor first on both -- are 64 and 43 checks,
none failed.
The thinkpad's Ironlake offers GL 2.1, and the context is refused with
`EGL_BAD_MATCH`; `fnl_init()` then fails, saying that
`LIBGL_ALWAYS_SOFTWARE=1` draws with Mesa's rasteriser instead, and with it
llvmpipe passes 65 and 44. That is the library's answer and not a fallback
of its own: Mesa is the one EGL with a software device, and on X11 and
Wayland it ignores `EGL_DEVICE_EXT` -- `dri2_setup_device()` replaces the
device asked for with the one it finds (26.0.8; checked, and the display
came back on radeonsi) -- so only the environment switches it, and the
library writes no caller's environment. The harness does, for the rounds
on a bare machine: a GL program there that exits at start with that
reason is started once more with the variable set (`_SoftGL` in
`environment.py`), and the device line then names llvmpipe.

### KMS: no window server

The fourth backend is none: a Linux console, where the program opens the
display's DRM node itself and takes one output whole. It is what an X server
with no window manager leaves out once more — the server — and it is the one
backend that runs where there is nothing to run under.

**The window is the output.** It opens at the connector's preferred
mode and is maximized from the first frame for its whole life:
`set_maximized` says maximized again, and minimize, move and resize do
nothing. So the titlebar offers neither minimize nor maximize — a button
that does nothing reads as a fault on a console — and fullscreen keeps the
group gap to close (`FnlBackendVtbl::is_output`, read by
`deco_button_rects()`; the floor stays the whole strip's, which no mode is
near). Fullscreen is the one state left, and it takes the titlebar away
and brings it back over the same mode. `grab_ring` is 0. The scale is 100%
unless the caller overrides it: a bare connector reports its millimetres,
a television reports them wrongly, and a density guessed from them would
be wrong where it matters most.

**Its size is a mode, and its output one of the card's.** Every connected
output of the card the program takes is read at open
(`card_read_outputs()`), each with its modes (`modes_read()`):
every one, largest first and of a size the fastest first, but two a list
would name alike -- one size, one refresh to the hundredth -- once, and
interlaced and doubled modes not at all, past 32 the smallest going but
never the preferred. The window opens at the preferred mode of
`FNL_KMS_OUTPUT`'s output, else the panel's, else the first; an interlaced
preferred mode gives way to a progressive one of its size. A window server
sizes a window by points; here a mode is chosen by value, refresh and all,
so the public calls are their own and not `fnl_request_size_pt()`, which
does nothing on KMS as it does nothing on any maximized window:
`fnl_mode_count()`, `fnl_mode_get()`, `fnl_mode_current()` and
`fnl_request_mode()` for the output the window is on,
`fnl_output_count()`, `fnl_output_name_at()` and `fnl_request_output()`
for which (1.1; elsewhere the mode list is empty and the outputs are the
window server's monitors, *The titlebar controls*). They name a mode by
value and an output by name, so a request
still means what it meant after the list changed. The titlebar's lists are
the same lists through the same calls: a mode box, "3840x2160 @ 30 Hz", in
place of the size box, and an output box beside it, both cut to the rows
that fit below the bar and slid to hold the one in force.

The output and the mode are one selection, packed in one word (`KMS_SEL`), so
the two threads never read the one without the other. The pump adopts it, hands
it over (`sel_want`), counts a surface remake (`app_surface_remake()`) and only
then publishes the size and the output; the render thread asks for the count at
the top of a turn and again after it latches the size (`surface_remake_owed()`),
so a latch that reads the new size goes round to the rebuild instead of building
a swapchain of that size on the old surface. That rebuild is what a display
surface refuses: on schenker's HDMI monitor, `vkCreateSwapchainKHR` answered
`VK_ERROR_INITIALIZATION_FAILED` at each switch to a smaller mode while the
order was publish, then count, since the publish's own signal woke the render
thread before the count moved; and the recovery's swapchain is the size the
surface names (`choose_extent()`, where the window is the output), not the size
last drawn. A display surface names its mode and its display, so on that path
the remake is the surface-loss rebuild (`remake_surface`, logged as what it is,
and without the reset's pause), device and the caller's `on_gpu_lost` and
`on_gpu_created` included, since the render pass is built on the surface's
format; another output is its display acquired in place of the one held, on the
same device (`take_display()`), the old released first. The copy path's headless
surface takes any extent: the swapchain follows the size as on a resize, and the
next frame frees the dumb buffers, makes them at the new size and sets the mode
on the new connector. A pick leaves the scene's pin alone: it is the scene's
size, not the output's. So the titlebar has no `Fs Upsample` box here
(`ctl_check_shown()`) -- with no size list setting the pin it could only hold
the scene at the size already in force -- and `fnl_set_scene_size_px()` is how a
program pins one.

An output the window leaves goes dark, so the monitor says it has no
picture rather than holding the last frame (`kms_output_off()`). On the
copy path its CRTC is switched off (`SETCRTC` with no framebuffer), which
also frees it for the next. On the display path the connector's DPMS
property is set off instead, which leaves the connector bound to its CRTC:
Mesa takes a connector's bound CRTC when it lights it again but keeps the
primary plane it chose the first time (`wsi_display_select_plane()`
returns the cached one whatever the CRTC), so a connector unbound by
`SETCRTC` got the first free CRTC beside its old one's plane, and the
kernel refused every swapchain after ("Invalid CRTC for plane", then
`VK_ERROR_INITIALIZATION_FAILED`). On schenker that was the panel, moved
to HDMI and back by the list and then unplugged while the window was on
it. Which CRTC drives an output is asked of the
kernel at the time (`connector_live_crtc()`), not read from the open's
guess: with only one output lit at open both guess the same CRTC, and
Vulkan's driver picks its own for a display it sets -- on schenker, HDMI
came back on pipe A after the panel had held it, with B free. The cursor
follows the same answer: after a switch the pump looks for the new
output's CRTC every 50 ms until the kernel names one (`cursor_recheck()`),
since the mode set is the render thread's and a pointer nobody moves would
leave the cursor on the guess. The other outputs keep what the console put
on them until the window has been there.

A selection that will not show is gone back from. On the copy path that is
the `SETCRTC` failing, or the new size's dumb buffers not to be had; on
the display path, a surface made again twice running for the same
selection with no present since -- the swapchain lost its surface before a
frame was out, twice, since once is what schenker's HDMI output does at
the first present after it is lit -- a mode Vulkan does not list, or an
output whose display the device will not take. A display mode the kernel
will not show is refused before any of that: Mesa test-commits each image
of a display swapchain as it makes it, so `vkCreateSwapchainKHR` answers
`VK_ERROR_INITIALIZATION_FAILED` and a present never comes. On a
selection that is not the last to have shown, `create_swapchain()` reads
that as the surface lost (`RendererState::swapchain_refusal_lost`, set by the
bridge), so it is the same twice with nothing shown; otherwise the frame
would ask for that swapchain again every turn and hold the window on a
mode that will never show. The last selection to have shown keeps the
plain retry, since there is nothing to go back to. The render thread marks the
mode, or the whole output, refused, makes the last selection that showed
instead, and wakes the pump, which adopts that one, warns once and
publishes it; a refused mode or output leaves its list and is not chosen
again for the program's life. `FNL_FAULT=kms-mode-refused` forces it,
and `kms-swapchain-refused` the swapchain's refusal: on schenker's HDMI
output (2026-10-18, `asan`), a mode and then the panel, each refused
twice and given up, the window back at 3840x2160 on HDMI.

**Monitors come and go.** The kernel says a connector changed with a
uevent, `HOTPLUG=1` from the card's DRM device, on the netlink group udev
itself reads; the pump reads it there, which needs no library, and reads
the card's connectors again (`outputs_rescan()`). An output whose monitor
went is marked gone (`outputs_gone`) and leaves both lists -- so is one
whose connector the card no longer lists, as a DisplayPort MST sink's goes
with its monitor, while one the kernel will not read is left as it was,
since a failed read is not an unplug, and a uevent lost to an overrun
(`ENOBUFS`) is taken for one of ours; one that came
is its old slot live again where it is the same connector with the same
modes, and a new slot otherwise, so a monitor plugged back is the output
it was and a request by its name still means it. Slots are only ever
added and never written once published, so the render thread reads them
without a lock: 32 of them, the open's and one for each other monitor
seen after. The window leaves an output that went for the first still
there, at its preferred mode; with none left it stays, draws nothing
(`kms_window_on_screen()`) and is on the first that comes back. Its own
output come back is a surface made again -- the CRTC may have gone dark
with the monitor -- and on the copy path the next frame sets the mode
afresh (`scan_resets`).

**A monitor that leaves is away first.** The kernel's word that an
output went is not an unplug yet: the output is *away* (`outputs_away`),
still listed, the window still drawing there if it was, and gone only
once it has been away `KMS_AWAY_S`, 5 s, or `KMS_AWAY_LIT_S`, 2.5 s, if
the window was on it. Back within that with the modes it had, it is the
output it was and nothing is said beyond one line the first time; the
window's own is set again, as one back from gone is. A move asked for
while an output is away is owed (`away_owed`) and made when it is back,
or dropped with a line if it is gone instead; the window whose own output
went goes to one neither gone nor away, or waits for one to come back.
The pump wakes when an output's time is up and reads the connectors
again, so a return no uevent reports is still seen.

That is because a monitor leaves the bus as it falls asleep and as it
wakes. On `black` (2026-10-26) the Samsung SyncMaster on HDMI-A-1, driven
as DVI, is gone for 2.4 s in every 16.7 s, from 18.6 s after its last
frame, for as long as it is dark; amdgpu's hotplug interrupt reports
each, and each return reads an EDID whose extension block is not there
(`EDID block 1 (tag 0x00) checksum is invalid`), which the kernel drops,
keeping the same 20 modes. The same with no fenestal at all -- a script
lighting it and turning it off -- and the same turned off either way,
`SETCRTC` with no framebuffer or DPMS. The iiyama on DP-3, dark for a
minute, never left; lit again, it was gone for 0.8 s from 3.7 s after,
then connected with no modes for 0.26 s, then back with its 39. Taken as
unplugs, the Samsung left the list for 2.4 s at a time and the round's
move to it for the GPU sections clicked into one of those gaps; and the
window, moved back to the iiyama after the GPU sections, was sent to the
Samsung by the iiyama waking, and stayed there 17 s, until another
monitor's uevent, since the read after the iiyama's last one found it
with no modes. The waits are over twice what was measured: a cable
pulled from a dark output leaves the list 5 s late, and a window whose
monitor is pulled waits 2.5 s to move.

Which monitor leaves, and when, is the monitor's own and even its input's.
The same Samsung on both its inputs at once -- HDMI-A-1 and DVI-D-1, each
input with an EDID of its own, the DVI one with the real size and serial
-- left from its HDMI input alone, and only while it slept: both inputs
dark, the HDMI one was gone on the same 18.6 s, 2.4 s and 16.7 s and the
DVI one never left in 70 s; either dark while the other had a picture,
neither left. The iiyama is the same on DP-1 as on DP-3: dark 100 s and
never gone, then gone 0.9 s from 3.7 s after it was lit again. With the
Samsung on DVI alone and the iiyama on DP-1, both KMS rounds pass whole,
the one line said for the iiyama waking after the GPU sections.

Driven (2026-10-18) on schenker by root writing `off` and `detect` to a
connector's `status` in sysfs, which sends the uevent a cable does
(`force_output()`, the round's `forced-hotplug`): the window's output
unplugged and it moves to the other, plugged back and it is listed again
as the one it was without the window moving, the other unplugged and it
comes home. The four rounds -- anv on the panel and on HDMI, NVK's copy
path on each -- pass whole, and by hand both unplugged at once (the window
waits) and then both back, at `release` and `asan` (the formats leak
below the only report). And with the cable, by hand, the window on HDMI:
pulled, and the window was on the panel at 1920x1080 at once; pushed back
in, HDMI listed again as the output it was, 32 modes, the window staying
on the panel; and pulled and pushed straight back, one unplug and one
plug, 3.5 s apart (the kernel's own wait for the monitor), the window
not moving. Nothing in the kernel's log, and the program left on Escape.

The force is the kernel's to deliver, and not every kernel does. On this
desktop's RX 580 (amdgpu, kernel 7.0.0-38) writing `off` and `detect` to
DVI-D-1's `status` sends no uevent at all -- `udevadm monitor -k -s drm`
saw none -- since every connector of that card is hotplug-capable, DRM
leaves output polling off there, and a forced status is queued as a
delayed event nothing sends; the window never moved, and a round's
unplug checks read a window that had not left. So the round listens for
the force's uevent (`Bare.force_output_heard()`) and skips the four checks
with that reason where none comes: three console rounds on `black` on
2026-10-24 skipped them so, and the thinkpad's i915 sends one for an `on`
and a `detect` of a connector with nothing on it.

Driven (2026-10-18) through the titlebar's lists by uinput, at `release`
and `asan`, and in the round (`display-modes`): on schenker, anv driving
the panel (eDP-1, one mode) and an HDMI monitor (32 modes of more,
preferred 3840x2160 at 30 Hz) -- 3840x2160 at 24, 29.97 and 30 Hz,
1920x2160 at 60, 720x400 at 70, each there and back; the window to the
panel and back, the one left dark (`active=0` in the kernel's state) and
the cursor on whichever CRTC drives the new one; with the fault forced, a
mode and an output refused and the window back on HDMI at 3840x2160 with
the warning. The copy path on `vkms`'s output on the thinkpad
(`Virtual-1`, lavapipe), since none of the laptops' panels lists a second
mode: 1024x768 to 1280x720 and back, and to 2560x1600 and back; with the
fault forced, 1280x768 refused and the window kept at 1024x768. A refusal
the display itself makes, rather than a fault's, is not seen on these
machines: the kernel prunes a mode the hardware cannot drive before it
lists it (a connector's `mode_valid`), so every listed mode there showed.
What Mesa does with one is read from its WSI (`main`, 2026-10-18): the
test commit at the swapchain's making, above, and a commit that fails at
a present anyway answered `VK_ERROR_SURFACE_LOST_KHR`.

A panel lists one mode, its own, and no scaled ones are made up beside
it. The display path could not offer them: Mesa's `vkCreateDisplayModeKHR`
hands back a mode only where the connector already lists one like it, so
a 1280x720 on a 1920x1080 panel would be the copy path's alone, through
the kernel's panel fitter, and the lists would differ by path. What a
smaller mode would buy is fewer pixels drawn, and the scene's pin
(`fnl_set_scene_size_px()`) gives that on both paths without a mode set.

Two things on schenker were seen once and not again. One start of the
`asan` testbed on the HDMI output exited with "device lost", after a
`release` round on the panel: 25 starts since, 5 of them after the panel
in that order, were clean. And one round started on the panel, in a boot
four hours old, had the panel fail link training every 6.5 s from the
hotplug section on ("Failed to enable link training", i915) and failed
*exits on Escape* and *log is clean* ("surface lost"); in a fresh boot,
the rounds and 15 cycles of both outputs unplugged and back under the
testbed left nothing in the kernel's log.

With the frames drawn by an NVIDIA GPU (`FNL_GPU=nvidia`, NVK), which
drives neither output there, the same is the copy path onto the Intel
card's two: on schenker's GTX 1050 Ti both rounds -- started on HDMI and
on the panel -- pass whole, *log is clean* included; HDMI to 1920x1080 and
800x600, the window to the panel and back with the one left dark, the
cursor on each, the frames on each (pictured), and the forced refusals, at
`release` and `asan`. Aspire's GTX 960M has its panel alone, and its round
passes with the two sections skipped.

Three things there were Mesa's (26.2.4, and `main`), all in its display
WSI and all drafted for it in `docs/mesa-wsi-display.md`: the plane kept
for a connector given another CRTC, above, and two more. That HDMI output
lost its surface at the first presents after it was lit -- at open, and at
each arrival on it -- and was rebuilt once, failing *log is clean*: the
first presents at 3840x2160 wait out the mode set, some 140 ms there (the
first atomic commit's time, under strace), the third acquire timed out at
our 100 ms, and `wsi_display_acquire_next_image()` reports a timeout as
`VK_ERROR_SURFACE_LOST_KHR` and breaks the swapchain, since it tests its
timed wait's C11 `thrd_timedout` against `ETIMEDOUT` (fixed upstream by
e102ea9f, in 26.2.6). No ioctl failed, and
under gdb, slower, it did not happen. On a display surface the acquire
therefore waits 2 s (`ACQUIRE_TIMEOUT_DISPLAY_NS` in `frame.c`): long
enough for a mode set, and still a bound for a display that stops. With it
the rounds there are clean at `release`, both outputs; the copy path never
met it, since there the backend sets the mode and the headless acquire
does not wait on a display.

And under `asan` each display swapchain destroyed and surface queried
again leaves 64 bytes behind: `wsi_display_setup_crtc()` allocates
`connector->formats` again once the last swapchain on the connector has
set `crtc_id` to 0, over the array already there -- one forced
`surface-lost` on the panel shows it too. A mode or output switch is such
a rebuild, so an `asan` round on the display path that switches fails *log
is clean* and *exits on Escape* (LeakSanitizer's exit status) with one
report a switch, and nothing on this side can avoid it but keeping
swapchains alive.

**The display is Vulkan's where a device can drive it, and the
backend's where none can.** Three ways were open; one was rejected, and
the other two are tried in turn:

* *The driver's own list* (`vkGetPhysicalDeviceDisplayPropertiesKHR`),
  rejected. A
  driver that offers `VK_KHR_display` opens the primary node for itself,
  and it can set a mode only as master; a program running as a user gets
  master only by being the first to open the node, and the backend has
  to open it first to read the mode the window is. Two opens, one master.
* *`VK_EXT_acquire_drm_display`*, first. The backend opens the node first
  and so holds master; `DRM_IOCTL_SET_MASTER` is then a no-op that says
  so, and fails — `EACCES` — while an X server or a compositor has the
  card, which is the message a user gets. Mesa's WSI gives up the
  descriptor it opened for itself when that one is not master, so the
  bridge lends it a `dup` of the backend's: one open file, so one master,
  and the driver may close its copy when the display is released without
  closing the backend's. One open file is also one event queue, which
  Mesa's WSI reads on a wait thread of its own for as long as it holds
  the copy (`wsi_display_wait_thread`, started at the first acquire that
  waits and stopped by `vkReleaseDisplayEXT`), and one set of client
  caps: the backend's descriptor carries `DRM_CLIENT_CAP_ATOMIC` from the
  moment the display is acquired, which the legacy calls it makes do not
  mind. The queue is the hazard. A flip of the backend's still queued
  when the copy is lent -- the copy path's or OpenGL's last, whose event
  is read only at the next frame without vsync -- is read by Mesa as one
  of its own images and its `user_data`, 0 for ours, dereferenced
  (`wsi_display_page_flip_handler2`, Mesa 26.0.8: a segfault in RADV at
  `image->chain` on this desktop's console, at the move from OpenGL back
  to Vulkan); and the driver's last flip, in flight when the display was
  released and the thread stopped, would count as the backend's first,
  and the flip after it find the CRTC busy and the copy path refused for
  good. So the backend waits its pending flip out and empties the queue
  before the descriptor can be lent (`present_window_renew`,
  `FLIP_DRAIN_MS`), and empties it again after each mode set, which the
  kernel completes only once every flip before it has
  (`events_drain()`). A DRM lease would give the driver a descriptor with
  a queue of its own and close the sharing for good; the `dup` stays
  because a lease names the CRTC and the plane before the driver chooses
  them, one lease per output the display moves to, for a hazard two
  drains close. `vkGetDrmDisplayEXT` names the display by the
  connector the backend chose, a hardware device is asked before a CPU
  one (a software device takes any descriptor it is lent), and the
  device that took it is the only one `pick_device()` may choose
  (`RendererState::surface_phys`). The mode is the one of that size with the
  nearest refresh, the plane one that can show the display and shows no other,
  and `vkReleaseDisplayEXT` hands it back before the instance goes. The display
  swapchain flips on vblank under FIFO, so the present is the throttle and
  `invites` is false, as on X11 and Win32.
* *A CPU copy into dumb buffers*, where no device will take the display.
  Feared as a second present path through the renderer seam, which every rule
  of *The frame* would have to hold for twice, and built without one: the
  bridge makes a `VK_EXT_headless_surface` instead, so acquire, present,
  rebuild and loss are the swapchain's as everywhere else
  (`RendererState::present_copy`). Each image's command buffer ends with a copy
  of it into a host-visible buffer, out of `PRESENT_SRC` and back; after the
  present the render thread waits for the frame's fence and hands the pixels to
  `FnlBackendVtbl::present_pixels`, the one seam entry it added. The backend
  copies them into whichever of two dumb buffers is not on screen and
  page-flips; the first frame sets the mode, so the console stays until there is
  a frame to replace it. Under vsync it waits for the flip's event, which makes
  it the throttle a FIFO present is; without it the flip is asynchronous
  (`DRM_MODE_PAGE_FLIP_ASYNC`), out at once and tearing as IMMEDIATE does,
  and a frame that still finds the last flip pending is dropped, as a
  mailbox replaces one (*Vsync off is FIFO on the display path*, below,
  has the driver's refusal and the fallback). Its buffers are
  device-scoped and sized for the largest extent yet, for the per-image instance
  buffers' reason. The bridge's extensions are optional for this: it asks for
  the display set and the headless one together, and takes whichever way the
  loader and the devices allow, saying which at open.

**No framebuffer is removed while a plane shows it, and the console is
given back at close.** The kernel removes a framebuffer that is shown by
disabling its plane (`atomic_remove_fb()`), and where the driver refuses a
CRTC with no primary plane -- amdgpu does -- by disabling the CRTC, which
on this desktop's AMD GPU fails as well: `atomic remove_fb failed with
-22`, a `WARNING` with a stack in the journal and the kernel tainted, once
per move away from Vulkan's display path and once per exit -- six in the
two runs of 2026-10-24 that also found the event queue above (kernel
7.0.0-38, Mesa 26.0.8). Mesa removes a display swapchain's framebuffers
as it destroys it, the one on screen included, so the bridge has the
backend take the output first (`VkBridge::scanout_take`,
`kms_scanout_take()`): a black dumb buffer set on the CRTC the driver
chose, at the mode read off that CRTC -- only a mode equal to the one set
makes the set a plane update and not a mode set, and the driver's pick of
the selection's size may differ from the backend's in its timings. Taken
so, the connector stays bound to its CRTC, which is the state Mesa's next
swapchain handles (`kms_output_off()` says what an unbound one does). The
backend's own pair is held to the same rule: a change of size makes the
new pair first and frees the old once the new is on the plane
(`scan_old`), and at close every CRTC the open found lit is set back to
what it showed then (`crtcs_save()`, `crtcs_restore()`) -- a
framebuffer's id names it to every client of the device, the console's
included -- so the console is on screen before the backend's framebuffers
go, and the kernel's own restore at the last close finds it done; a CRTC
the console will not come back on is turned off, and that restore lights
it. A retired swapchain is freed only after further frames presented on
its successor (`vk_retire_tick()`), by when the successor's image is the
one shown.

That left one path, which two console rounds in three still met at the
move back between outputs: the output the window leaves goes dark by
DPMS, its CRTC inactive and the backend's black frame still on its plane,
and the take-back on the other output, at another size, retired that pair
and freed it -- a framebuffer shown, if only on a CRTC that is off, and
the same refusal. Equal sizes reuse the pair, which is why one round in
three logged nothing. So a pair is never freed while any CRTC shows one
of its buffers (`scan_shown_on()`): it is parked instead
(`scan_release()`), and freed once nothing shows it, or at close after
`crtcs_restore()`, its CRTC turned off first where nothing set that one
back. Measured on `black` (2026-10-24, HDMI-A-1 at 3840x2160 and DVI-D-1
at 1920x1200): three Vulkan rounds and an OpenGL one, each through the
output list and back, and no `remove_fb` failure, `WARNING` or taint in
the kernel's log, where two rounds of three had logged one before. What
amdgpu still logs, once a round, is `REG_WAIT timeout ...
program_formatter_reset_dig_resync_fifo` at a mode set, which the
console's own mode set at boot logs as well.

**Input is evdev, read by the backend.** Every `/dev/input/event*` the user
may open is classified by what it says it reports: a keyboard has the
letters, a mouse relative X and Y and a left button, a touchpad absolute
axes and a finger tool, and anything else absolute with a button is a
tablet or a touchscreen, mapped onto the output. A touchpad is read as
relative motion, a millimetre of finger to eight pixels, unaccelerated; no
tap, no two-finger scroll. The three laptops' pads all give a resolution
(schenker's 108 x 59 mm, aspire's 103 x 75, the thinkpad's 78 x 40), so a
stroke the pad's width is 866, 824 and 626 pixels, a little under half of
each panel. A uinput pad at each one's units a millimetre, dragged under
the testbed on KMS, moved the pointer exactly: 10 x 5 mm to 80 x 40 px on
schenker, 20 x 10 mm to 160 x 80 on aspire, 15 x 7 mm to 120 x 56 on the
thinkpad (2026-10-18). Nothing is accelerated: a curve is a policy, and
libinput's would be a library on the link line. `inotify` on `/dev/input`
finds a device plugged in later, on `IN_ATTRIB` as well as `IN_CREATE`,
since a new node is not the user's to open until udev has set its group.
`SYN_DROPPED` throws the rest of the report away and reads the keys held
afresh with `EVIOCGKEY`, as focus arriving does on the other backends.

**The keyboards are grabbed** (`EVIOCGRAB`). The console's tty reads the
same devices, and a key that reached it as well would be typed into
whatever is logged in there — a getty, or a root shell. A grab is the one
way to keep it out that needs no tty of the program's own; the cost is
that the console switches nothing while the program runs, and that a
program that hangs keeps the keyboard until it is killed.

**The layout is read from xkeyboard-config's own files.** There is nobody
to hand over a keymap, so the console's is built from what names it --
`XKBLAYOUT`, `XKBVARIANT`, `XKBOPTIONS` and `XKBMODEL` in Debian's
`/etc/default/keyboard` or systemd's `/etc/vconsole.conf`, or the latter's
`KEYMAP` through systemd's `kbd-model-map`, which is what an Arch console
has -- the way libxkbcommon builds one for a compositor, with nothing run
and nothing linked (`src/backend/keymap_rules.c`). The names go through
`rules/evdev`, whose rows turn model, layout, variant and options into two
component chains, `evdev+aliases(qwertz)` and
`pc+de(nodeadkeys)+inet(evdev)`: a set of rows under a header that names
the columns matched and the component filled, a cell a literal, a `$set`
or `*`, `%l`, `%v`, `%m` and `%(v)` in a value the names, a value that
begins with `+` appended and one that does not the component while it is
empty, a set's first match its last except where options are matched,
and the unindexed `layout` column for one layout and `layout[N]` for
several -- libxkbcommon's reading (`rules.c`), which the sweep below
holds the reader to. Each piece of a chain is a section of a file under
`keycodes/` or `symbols/` that says `include "latin(type4)"` for more,
and `keymap_xkb.c`'s tokeniser, the one that reads a compositor's text,
follows those includes: the included section first, so the including
one's own keys land over it, which is `override`, what `+` means and what
every chain the rules write uses; `|` is `augment`, which fills only what
is empty; a `NoSymbol` leaves its level as it was under either, which is
how `eurosign(e)` adds a third level and keeps the two below; `replace`
starts a key over; `VoidSymbol` (`none`) fills a level with nothing, so
that what the base had there stays covered, where `NoSymbol` (`any`)
leaves it, and so does an `XF86*` media key, whose names are another
header's and type nothing; and `:2` on a piece puts it in the second
group. Of a key's
type only the level count is read -- `type[Group1]="TWO_LEVEL"` on the
key, or a section's `key.type` default -- and only to cut the levels a
type with fewer than four has no room for, which is what the compiler
does with them. A type named for a group is that group's alone and the
unindexed one every other group's, so a key can be two levels in one
group and four in the next; and a piece's types reach only its own
groups, never below the `:2` it was included at; compat and geometry are
not read at all, since a key's type decides nothing else here (*Text
entry*). Bounds, though the files are root's: fifteen levels of include
(libxkbcommon's own), 256 pieces a parse, a megabyte a file and four in
all, and a name that would leave the root refused. A cycle is a depth
that runs out; a file or section not there contributes nothing and is
one line at open, naming it -- unless it is a layout's own, which is no
layout: a typo in `XKBLAYOUT` types nothing rather than `pc`'s few keys.
The modifiers are worked out from the keys held, since nobody reports
them: AltGr is the right Alt where the keymap's plain level there is
`ISO_Level3_Shift`, and Alt otherwise, and the two locks are toggles the
backend keeps and writes to every keyboard's lights. A held key repeats
by the kernel's own repeat (value 2), which goes through
`app_key_repeat()` as every backend's does.

It had been `xkbcli compile-keymap`, run with a five-second bound and its
text read as a compositor's. That is libxkbcommon's tool, and on
2026-10-24 this desktop's testbed said `no keymap for layout de: xkbcli
did not run`: Debian and Ubuntu ship it in `libxkbcommon-tools`, which
nothing installs by default, and the harness had been installing it on
the laptops by hand; Arch ships it inside `libxkbcommon`. The data the
tool compiles is on every one of those machines regardless -- `xkb-data`
is a dependency of the package that writes `/etc/default/keyboard`, and
`xkeyboard-config` of Xorg and of libxkbcommon -- so the data is the
dependency now: `XKB_CONFIG_ROOT` where it is set (libxkbcommon's and
`xkbcomp`'s variable, which is how NixOS finds it), else
`/usr/share/X11/xkb`, else `/usr/share/xkeyboard-config-2`. Without the
rules file the components are what every PC model's rows come to;
without the data keys still arrive, text entry types nothing, and one
line says what was not found. Considered and left: opening libxkbcommon
itself at run time through `dynlib.h`, five entry points and an
afternoon, which moves the dependency rather than removing it -- the
library is a toolkit's, installed with GTK, Qt or a compositor and absent
on the console-only machine this backend is for, where the data is
present whenever the configuration file is; `xkbcomp` as a second tool,
which takes the component block but wants the rules read first, after
which nothing is left for it to do; the kernel's own table (`KDGKBENT`),
which wants a console tty the program is built not to have and is a
translation of the XKB one with gaps of its own; and a table of layouts
baked at build time, a shipped list of what is derived on the target
already.

`FNL_KMS_LAYOUT=de(nodeadkeys)` names the layout in the console's place,
`de(nodeadkeys),us` two groups and `de caps:none` the XKB options after a
space -- the four names as one line, the model staying `pc105`, since for
the keysyms it decides nothing but Apple's and Sun's own files; the
testbed's `--layout=` sets it. This backend's alone: under a window
server the layout is the session's, set where the session sets it, and a
program there with the variable in its environment is told so in one
line and types through the session's. The user's own `~/.config/xkb/`,
which libxkbcommon consults ahead of the system's tree, is not read: a
console machine's layout is the system's.

**Group keys.** The backend keeps the group itself, as it keeps the locks,
and a press of `ISO_Next_Group`, `ISO_Prev_Group`, `ISO_First_Group` or
`ISO_Last_Group` (or a `_Lock` of one; XKB's compat map makes every one a
lock) moves it, saying `KMS: typing in group 2 of 2, us`
(`keymap_group_key()`). Which press is one is where the type rule above
gives way: xkeyboard-config's `grp:` options put `ISO_Next_Group` on a
key's second level under another modifier than Shift --
`grp:alt_shift_toggle` writes Left Shift as `[Shift_L, ISO_Next_Group]`
of type `PC_ALT_LEVEL2`, so Shift alone is Shift and Alt+Shift switches --
and read by the four-level rule, Shift alone would switch. So the parser
keeps one thing of a key's type besides its level count: which modifier
selects its second level, Alt, Ctrl or Super for the three `PC_*_LEVEL2`
types and Shift for every other; and that is read for a group key alone.
`ISO_Group_Shift` and `Mode_switch`, which latch only while held, are not
acted on. The round's KMS environments start the program with the
console's layout, `us` and `grp:alt_shift_toggle` (`group_layout()` in
`environment.py`), and check that the Y key types `y` after Alt+Shift and
the console's own letter after the next: passed on the Pi (`de`, V3D) and
the thinkpad (`de`, lavapipe) at the first run, and on `black` (`de`,
RADV and radeonsi) in four console rounds.

`ISO_Level3_Shift` prints nothing, so the keysym name table
(`tools/mkkeysyms.py`) had left it out, and `keymap_xkb.c` read right Alt
as an empty key: AltGr was Alt, a shortcut, and typed nothing. The table
keeps the name now and `verifykeymap` holds the symbols files' spelling
of the key (`level3(ralt_switch)`) to it. `verifykeymap` also writes a
miniature tree of its own -- an include chain, a named section beside a
default one, a cycle, a missing file, a key redefined by the includer,
the three merges, `:2`, rules with a `$set`, indexed columns and options
-- and resolves names against it, against the data of the machine it
builds on where that is there, and against damaged copies of both.
`tools/probe/keymapdump.c` prints the keymap for a layout either way,
from the files or from a compiled keymap's text, and `xkbdiff.py` runs
every layout and variant `rules/evdev.lst` names through both and prints
each key the two type differently. Run on 2026-10-24 on schenker
(xkeyboard-config 2.48, libxkbcommon 1.13.2's `xkbcli`): 598 pairs, none
differ, after the four defects the first run found -- the name table's
missing deprecated aliases, `VoidSymbol` read as a level left out, the
short spelling `U458` refused, and no cut to a type's levels -- each a
`verifykeymap` case now. Against X.Org's `xkbcomp` on this desktop 18
pairs still differ, every one `xkbcomp` cutting a merged key to the
levels the overriding file wrote where libxkbcommon keeps the base's, so
`xkbcli` is the reference and `xkbcomp` the stand-in where it is not
installed. The same day the three laptops' KMS rounds typed German's 23
sequences through the files, every check passing, and the override and
its refusals were seen in the log on two of them; and this desktop's own
console boot (amdgpu, `de` `nodeadkeys` in `/etc/default/keyboard`, the
configuration that had found no `xkbcli`) typed its 21 -- the acute and
grave key as spacing marks, the circumflex key its sign -- with Vulkan
and with OpenGL, driven as a bare machine of the harness's. The round
types the console's layout where it has a table for it
(`known-layout`, `LAYOUT_TYPES` in `stress.py`): for `de`, seventeen keys
and chords -- y and z swapped, ß, the umlauts, the 102nd key's `<>|`,
AltGr's `@€{\`, Shift's `§/°` -- and six dead-key sequences, the acute
and grave on one key, the circumflex, an accent alone by Space or by the
same key twice, and a letter it does not go on. All 23 type as the
legend says on all three of the thinkpad's servers: KMS; weston, which
shares the composer and gets the layout through `weston.ini`; and Xorg,
through `x11_keys.c`'s own XKB reading. There the harness types through
XTEST, whose keyboard has the server's default keymap, US -- left so,
every key typed its US legend -- so `xtest_layout()` gives that device
the console's layout by `setxkbmap -device` as the server comes up, and
the core keyboard takes it when XTEST first types.

**The pointer is drawn on the cursor plane.** Nothing else draws one on a
bare output. The legacy cursor ioctl takes a dumb buffer of the size the
driver asks (`DRM_CAP_CURSOR_WIDTH`, 64 by default) and moves it without a
frame; the CRTC is the one driving the connector, found once there is one,
which is the one Vulkan keeps. There is no theme to load shapes from, so
three are drawn from text in `kms.c` — the arrow, the I-beam and the
crosshair — and the rest of `FnlCursor` is the arrow, which says less and
nothing wrong. An output that refuses a cursor is said so once, and the
pointer is then not shown. A cursor goes on a lit CRTC only
(`crtc_lit()`): the copy path switches off the CRTC of an output it leaves,
and coming back the open's guess of a CRTC is off until the render
thread's mode set, where amdgpu refuses a cursor (`EINVAL`) -- which, taken
as the output's answer, hid the pointer for the window's life after the
console's OpenGL round moved back. The recheck after a switch sets it once
a CRTC drives the output. On `black`'s OpenGL round since (2026-10-24):
no refusal logged, and the pointer seen in the scanned-out frame after the
move back, the arrow's 69 white and 50 black pixels where the echo put it.

The round looks at it (`drawn-pointer`, a capability KMS alone declares):
`kms_grab.py` sets the atomic client capability, which is what shows a
plane's `CRTC_X` and `CRTC_Y`, finds the cursor plane on the CRTC by its
`type`, reads the framebuffer the kernel wrapped the legacy buffer in, and
blends it over the primary plane's picture. Pictured with the pointer at
a third and at two thirds of the window's width, the arrow's box holds
exactly the glyph's 69 white and 50 black pixels where the echo says the
pointer is, and none at the other spot: 40 checks on the thinkpad, none
failed.

**The clipboard is the window's own**: there is no other program to share
one with, so a paste answers with what the window last copied.

**On the thinkpad it is the copy.** Its Ironlake graphics no Vulkan
driver covers, and lavapipe (Mesa 25.0.7) answers `vkGetDrmDisplayEXT`
with `VK_ERROR_EXTENSION_NOT_PRESENT` and lists no display of its own: the
loader advertises `VK_KHR_display` and the acquire extension for the
machine's other drivers, and the entry-point names in lavapipe's library
are Mesa's shared tables, not support. So the line at open is

    KMS: no Vulkan device would take LVDS-1: vkGetDrmDisplayEXT on llvmpipe
    (LLVM 19.1.7, 128 bits) returned VK_ERROR_EXTENSION_NOT_PRESENT (-7);
    drawing offscreen and copying each frame onto LVDS-1

and the testbed draws its moving scene at 30 fps at 1366x734 under FIFO,
read off its own counter in a picture of the panel. The picture is the
framebuffer the CRTC is scanning out, read by root (`kms_grab.py`), which
is what the round takes as `composited`: 39 checks (40 with the
pointer's, above), none failed, five not applicable, the titlebar seen on
the screen itself, gone in fullscreen and back; `asan`, `tsan` and `gc`
are the same 39 each, none failed, under the validation layer, which is
the readback's barriers checked too.

Where the CPU may not map the framebuffer, the GPU reads it. amdgpu
refuses both `MAP_DUMB` and the mmap of the PRIME-exported dma-buf
(`EPERM`) for an image made in VRAM with no CPU access -- RADV's scanout
images on KMS, and mutter's -- so `kms_grab.py` then exports the image,
imports it into Mesa's EGL with its format, modifier and every plane, and
draws it into an 8-bit framebuffer of its own to read back
(`gpu_read()`), through ctypes and the machine's `libEGL` and
`libGLESv2`, Mesa's vendor alone since NVIDIA's would be asked about an
AMD or Intel buffer. Drawn rather than read where it is: an imported
image need not be one GL renders to, and mutter's 10-bit
XRGB2101010 scanout is incomplete as an attachment, while every image a
driver imports it samples. On this desktop, RADV's 3840x2160 scanout on
the console and mutter's under GNOME, 0.3 s a picture, and whole: the
copy is one GPU pass, so no flip lands in the middle of it as one can in
a mapped buffer read row by row.

`pace.py` has the thinkpad's three servers side by side, release, 12 s a
cell, CPU over 6 s of it, `MESA_VK_WSI_DEBUG=noshm` for Xorg and
`repaint-window=10` for weston (below):

| sizes/s | `bare@thinkpad` (Xorg) | `weston@thinkpad` | `kms@thinkpad` |
|---|---|---|---|
| idle | 0 fps, 0.3% | 0 fps, 0.2% | 0 fps, 0.2% |
| 5 | 20 fps, 21.4% | 20 fps, 22.5% | 0 fps, 0.2% |
| 15 | 20 fps, 67.1% | 30 fps, 70.5% | 0 fps, 0.3% |
| 30 | 60 fps, 97.6% | 60 fps, 120.4% | 0 fps, 0.3% |
| 60 | 60 fps, 106.3% | 60 fps, 140.3% | 0 fps, 0.5% |
| 250 | 60 fps, 117.0% | 60 fps, 141.2% | 0 fps, 1.8% |
| border | 42 fps, 270.8% | — | — |

The KMS column is the window that is its output: every size asked for is
refused, so nothing changes and nothing is drawn, and the column says only
that asking costs under 2% of a core at 250 a second. weston follows Xorg
up the rates on the same lavapipe, at a fifth more CPU at the top, and
at 15 a second it presents half as many frames again for the same CPU.
Neither has a border row: the harness drags no edge there.

**The copy path costs what Xorg's present costs.** `draw` is the row that
times a frame rather than a size: `fenestal-pace --draw` asks for every
frame and no size, so each is recorded and presented. Idle and draw, every
column, release:

| | idle | draw |
|---|---|---|
| `wayland-vulkan` | 0 fps, 0.3% | 59 fps, 6.3% |
| `x11-vulkan` | 0 fps, 0.3% | 60 fps, 6.2% |
| `win64-vulkan` (wine) | 0 fps, 0.7% | 60 fps, 7.8% |
| `win64-vulkan-wl` | 0 fps, 0.7% | 59 fps, 8.0% |
| `@schenker` (GTX 1050 Ti) | 0 fps, 0.5% | 60 fps, 6.5% |
| `@aspire` (GTX 960M) | 0 fps, 1.0% | 60 fps, 6.2% |
| `bare@thinkpad` (Xorg, `noshm`) | 0 fps, 0.3% | 60 fps, 70.9% |
| `weston@thinkpad` | 0 fps, 0.2% | 60 fps, 61.8% |
| `kms@thinkpad` (copy path) | 0 fps, 0.2% | 60 fps, 68.7% |

On the thinkpad the copy path holds the panel's 60 Hz at 68.7% of a core,
against Xorg's 70.9% for a smaller window (1280x754 to the output's
1366x768): the readback, the fence wait and the flip cost no more than
Mesa's `noshm` present into an X server. Most of either is lavapipe's
frame, and weston's 61.8% is the same frame handed over as `wl_shm`.

**The display path, measured on the laptops (2026-11-05, `bench.py
--hands`, three runs a machine, Intel's GPU on its own panel at
1920x1080):** 60.0 fps with p99 16.8 ms and a 1% low of 59, where the
copy path's flips read 58.5 to 59.2 with p99 33 and a 1% low of 30;
2.70 to 2.73 ms of GPU and 0.6 to 0.7 of CPU a frame against the copy
path's 2.29 to 8.64 and 2.4 to 2.9; and 20.5 to 20.6 ms from latch to
screen against the copy's 8.7 to 12.7. The two ways trade: the queued
frame costs the display path the quickest present and buys it a cadence
with no doubled intervals and a quarter of the CPU, which is the
readback the copy pays. The GPUs differ with the way -- only Intel's
takes the display, since NVIDIA's 580 names none beside Intel's panel
(*Moving to another GPU*) -- so the GPU figures read as the device's
and the CPU and cadence as the path's.

OpenGL's column on the copy path (2026-10-31, `1d987ac`, aspire's GTX
960M on NVIDIA's driver onto Intel's eDP-1, release, the ranking set
aside): `kms-gl@aspire` draws 60 fps at 33.7% of a core where Vulkan's
`kms@aspire` beside it draws 58 at 18.0% -- NVIDIA's OpenGL frames reach
the Intel panel through the CPU (*KMS: no window server*), nearly twice
Vulkan's copy. A column measures the renderer and GPU it names, ranked
machine or not: the harness drops both for a ranked round on a kernel
driver, so `pace.py` asks for an unranked one, as `bench.py` does --
before it did, a ranked machine's `kms-gl` column was Vulkan's.

**Vsync off is FIFO on the display path.** Mesa's display WSI offers one
present mode: `wsi_display_surface_get_present_modes()` appends
`VK_PRESENT_MODE_FIFO_KHR` and nothing else (read on Mesa's main; the
WSI is common to Mesa's drivers, and `anv` is the one measured). The
vsync-off diagnostic asks for MAILBOX, then IMMEDIATE, and keeps FIFO where
neither is offered (`choose_present_modes()`), so a display surface
presents at the refresh
whatever `FNL_VSYNC` says. Measured by `fenestal-pace --draw
--no-vsync`, which names the mode it ended on: `kms@aspire` and
`kms@schenker` 60 fps at 1.5 and 4.1% of a core, the 16.5 ms a frame all
in `wait` -- FIFO's acquire -- and none in the frame policy's pacing, with
`FIFO` on the closing line. The copy path is not held: its surface is a
headless one and the flip is the backend's, and `kms@thinkpad` reached
184 fps at 217% on lavapipe. `fnl_present_mode()` says `FIFO` on the
display path while `FNL_VSYNC` says off, which is the two answering
their own questions: what was asked for, and what the surface gave.

Two ways to an uncapped display path were set aside. The copy path while
vsync is off -- a headless surface drawn flat out and the newest frame
flipped at each vblank, mailbox in effect -- costs a readback a frame the
display path exists to avoid, for a mode nothing here needs on a console.
And an IMMEDIATE that tears wants `DRM_MODE_PAGE_FLIP_ASYNC` in Mesa's WSI,
which is Mesa's to add; vsync off takes it unchanged the day it is
offered (not on 26.0.8, not on main, 2026-10-24).

The backend's own flip, the copy path's and OpenGL's, is asynchronous
without vsync (`DRM_MODE_PAGE_FLIP_ASYNC`): the frame goes out at once and
tears, as IMMEDIATE does on a window server, and a frame that finds the
last flip pending -- the drop -- is the rare case rather than the rule.
The flag is the legacy flip's, which `drm_mode_page_flip_ioctl()` refuses
with `EINVAL` on a driver without `async_page_flip`, and which amdgpu's
atomic check refuses for an update that is not fast -- a change of pitch,
format or rotation, which a flip between two dumb buffers of one size
never is. Refused, the backend flips at the vblank until the next mode
set, where it asks again, with one line (`flip_async_off`). Vulkan's
display path is untouched: FIFO is what Mesa offers.

**The live key on the same machine.** The thinkpad booted from
the generic live key, since retired for the hosts' own sticks -- Arch,
kernel 7.2.9, Mesa 26.2.4 with LLVM 23, Xorg 21.1.25, weston 15.0.1,
against Debian's 6.12, 25.0.7, 21.1.16 and 14.0.2
-- runs the three rounds as Debian does: Xorg 69 checks, weston 51, KMS
42 with `--capture`, none failed. Mesa 26.2.4's lavapipe has no display
either (`VK_ERROR_EXTENSION_NOT_PRESENT`), so KMS took the copy path
there too. One difference reached the harness: systemd 262 refuses a
`systemctl stop` naming a unit that is not loaded and then stops none of
those named, so weston, stopped beside Xorg's collected unit, held the
display; `bare_display()` stops each by itself.

**The display path, on schenker's Intel GPU.** The live key on a Clevo
P64 (Kaby Lake HD 630 beside a GTX 1050 Ti, the panel on `i915`'s card2,
nouveau's card1 with nothing connected) is the first machine where a
device takes the display: `anv` acquires eDP-1, no copy path, and the
round is 42 checks with `--capture`, none failed. NVK answers the
display queries with `VK_ERROR_OUT_OF_HOST_MEMORY` (vulkaninfo stops
there), and the library goes on to the next device. Two things in the
grab came with it: the panel is not card0, so `kms_grab.py` tries each
card; and `anv` scans out Y-tiled images (modifier 0x100000000000002),
which it now reads tile by tile, X tiling too, through GETFB2 (the old
GETFB refuses a framebuffer made with modifiers); a compressed layout --
weston's, Y tiling with CCS -- is refused by name. With two outputs the
first lit CRTC need not be the window's -- with the window on HDMI and the
console still on the panel it was the console's framebuffer, which does
not map (`ENODEV`) -- so the harness names the output the window is on
(`--output`), and the grab follows that connector to its CRTC; a CRTC left
off by DPMS still names its last framebuffer, and is passed over by its
atomic `ACTIVE`.

**The display path shows unfinished frames there.** With the scene moving,
the panel carries rows of frames the GPU has not finished: lines through
the star where its field is drawn and its wedges are not, and the help
panel's background and text missing across its first rows. At rest the
screen is whole, and three grabs of it agree, so it is the scanout and not
the grab. Eight grabs a setting:

| | star lines | panel rows |
|---|---|---|
| two images, as shipped | in 7 | damaged |
| the frame's fence waited before the present | in 0 | damaged |
| three images | in 0 | damaged |
| both | in 0 | rows 62 to 70 only |

The synchronisation layer finds no hazard in the library's commands, and
Mesa 26.2.4's display WSI attaches the render semaphore to the image's
dma-buf before its atomic commit and marks the images scanout. The
library's cursor is not it: with `kms-no-cursor` the counts are the same.
Nor does the program draw into the image on the screen -- an instrument
that learnt each image's framebuffer from the CRTC found none of 358
acquires handing back the one being scanned out.

So on a display plane the frame's own fence was waited for before the
present, and the swapchain made three images deep (both since taken out
again, below).
That takes the star's lines from seven grabs of eight to none, costs
nothing `pace.py` can see (60 fps, 6.6% of a core, as on every other
GPU), and the round is 42 checks, none failed. What it leaves: a row or a
few of a frame finished before its flip that the display still shows
without the overlay over it -- the scene's ramp through the stats panel,
the sweep through the help panel's heading. Rows the GPU wrote and the
display has not seen are what a scanout image rendered through the
last-level cache, which the display engine does not snoop, would give;
that is the driver's to choose, not the library's, and it is owed to
Mesa (`TODO.md`). None of this is NVK's, whose frames under Xorg read
clean off the scanout in twelve grabs.

**The stray rows were the grab's.** `kms_grab.py` decoded the
framebuffer straight out of its mapping, slice by slice, for 3.6 s, and
the image it reads goes back into the swapchain and is drawn again many
times over: what it put together was rows of frames a whole cycle apart.
The steps say so. The testbed's sweep bar moves 8 px a frame at 60 Hz, and
a bar broken across rows is broken by 24 px on `anv` (three images) and
by 16 px on the NVIDIA copy path (two dumb buffers) -- one turn of the
buffers each, never one frame. And a grab that cannot race shows none:
the help panel's tiles alone (`x < 288`, rows 32 to 384, 12 slices of
Y tiling) read in 1.7 to 2.6 ms, kept only when the CRTC showed the same
framebuffer before and after, against the full grab, alternating in one
run on schenker's HD 630 with the scene moving:

| | grabs with stray rows in the help panel |
|---|---|
| full grab, decoded out of the mapping | 8 of 8 |
| the panel's tiles, 2.5 ms, steady | 0 of 8 (and 0 of 24 in a run of their own) |

A row counts as stray where more than 20 of its pixels differ from the
per-pixel median of the run's grabs. Mesa main (git 335d5698,
2026-10-17, `anv` alone in `/opt/mesa-git` on schenker's stick, chosen by
`VK_DRIVER_FILES`) gave the same 8 of 8 as 26.2.4 by the full grab,
which is the same finding: the rows in the help panel were the grab's
race, and the measures made it rarer. `kms_grab.py` now
copies the mapping out whole first, about 30 ms, and decodes the copy:
the same eight grabs with the scene moving have no stray row and no
broken bar, and the KMS round with `--capture` is 42 checks, none
failed, on schenker's HD 630 and GTX 1050 Ti. 30 ms is still two frames,
so a picture of a moving scene can yet be two of them, rarely.

**Nor is the fence wait the display's: the CRTC names a frame before it
is shown.** With each measure switched off in a build of its own, the
fast grab -- the help panel's tiles, and the stats panel's (`x < 352`,
rows 960 to 1088) the same way -- read with the scene moving on
schenker's HD 630:

| | help panel | stats panel |
|---|---|---|
| fence wait, three images (as shipped then) | 0 of 80 | 0 of 80 |
| no fence wait, three images | 6 of 160 | 4 of 160 |
| no fence wait, three images, Mesa main | 1 of 80 | 0 of 80 |
| fence wait, two images | 0 of 16 | 0 of 16 |
| neither | 0 of 16 | 0 of 16 |

The rows without the wait are the program's frame half made, and a
framebuffer watched from the moment GETCRTC names it changes under the
reads in every frame of 1801 -- but GETCRTC names the framebuffer of the
last *commit*, not the one being scanned out. The kernel's own events
(`i915:i915_request_add`, `dma_fence:dma_fence_signaled`,
`i915:intel_plane_update_arm`, on CLOCK_MONOTONIC beside the reads) put
it in order, the same for the testbed and for a plain Vulkan program on
`VK_KHR_display` presenting 1.3 ms before a vblank:

| ms | |
|---|---|
| 0.0 | the frame's request submitted |
| 0.8 | GETCRTC names its framebuffer |
| 1.2 | the framebuffer's pixels change under the reads |
| 3.4 | the request's fence signals |
| 3.45 | the plane armed (scanline 159): on screen at the next vblank |

The commit worker waits for the fence (`dma_fence_wait_start` on the
frame's own seqno) before it arms the plane, every frame; the implicit
sync `anv`'s WSI sets up -- the render semaphore's sync file imported
into the image's dma-buf before the atomic commit -- holds. What the
grabs read was a frame committed and not yet shown; with the library's
wait the commit comes after the GPU, so there was nothing to catch.
Nothing here is owed to Mesa, and neither the fence wait nor the third
image is needed for what reaches the panel. A grab of the CRTC's
framebuffer (`kms_grab.py`) is a grab of the last commit, which on a
display plane is the frame on screen only once its fence has signalled.

So both are gone again (2026-10-18). `pace.py` on the display path,
before and after: schenker's HD 630 60 fps at 7.0% of a core and 7.1%,
aspire's HD 530 at 3.3% and 4.1%. KMS rounds with `--capture`, 42 checks,
none failed: schenker on the HD 630 and the GTX 1050 Ti, aspire on the
HD 530 and the GTX 960M, the thinkpad on lavapipe. Without the wait a
grab of the moving scene can read a frame committed and still being
drawn; the round's captures are of the scene at rest.

**A frame from the other GPU comes by the copy path.** The bridge gave
the display to the first device that took it, and a surface on a
device's display is that device's alone, so with `FNL_GPU=nvidia` on
schenker the name found nothing to choose among and the program stayed on
the HD 630. Rendering on the GTX 1050 Ti and showing it on eDP-1 is the
copy path's job -- a headless surface, which NVK presents to like any
device, each image read back and written into `i915`'s dumb buffer -- and
nothing of it was lavapipe's but one choice: `create_instance_buffers()`
took the first `HOST_VISIBLE|HOST_COHERENT` type, and on NVK that is the
window onto VRAM, which the CPU reads uncached. `tools/probe/kmscopy.c`,
1920x1080 B8G8R8A8, medians of 20, ms, by glibc's `memcpy` as the copy
path copies (streaming loads would read the window faster, not usably
so), the one pass first and so cold:

| NVK memory type | GPU copy into it | CPU read | into the dumb buffer | read and write in one pass |
|---|---|---|---|---|
| `DEVICE_LOCAL HOST_VISIBLE HOST_COHERENT` | 2.4 | 541 | 0.7 | 425 |
| `HOST_VISIBLE HOST_COHERENT HOST_CACHED` | 6.8 | 0.7 | 0.8 | 1.1 |

(the HD 630 itself: 5.4 to copy, 0.7 in one pass). So the readback asks
for a `HOST_CACHED` type first and takes the old one where a device has
none; a buffer the CPU only writes -- the overlay's and the text's
instances -- keeps the first, which is where writes are fastest. Still
coherent: nothing invalidates. And a device chosen by name is the only one
asked to take the display where a device carries the name; when it will
not, the frames go by the copy path, where `pick_device()` chooses among
all of them, and the log says which device refused. An index is read the
same way, as the device in that place of the driver's enumeration -- the
order the copy path's list counts in, every device presenting to a
headless surface -- so on schenker `FNL_GPU=1` is the GTX 1050 Ti by the
copy path and 2 the HD 630 on its display; a name or an index that
chooses nothing, or a loader without the headless surface, leaves the
display to whichever device takes it, as before. The KMS round with
`--capture`, 42 checks, none failed, on each of schenker with
`FNL_GPU=nvidia` (the GTX 1050 Ti, by the copy path), with `FNL_GPU=intel`
and with nothing named (the HD 630, its display path), aspire with
`FNL_GPU=nvidia` (a GTX 960M, NVK GM107) and the thinkpad (lavapipe);
under asan, 38 without the pictures, on schenker's NVIDIA and the
thinkpad. NVK refuses the display with `VK_ERROR_UNKNOWN`, which the log
now names. `pace.py`'s draw row, release, two sweeps agreeing:

| `kms@` | idle | draw |
|---|---|---|
| schenker, `FNL_GPU=nvidia` (copy path) | 0 fps, 0.2% | 60 fps, 16.2% |
| schenker, `FNL_GPU=intel` (its display) | 0 fps, 0.2% | 60 fps, 6.5% |
| aspire, `FNL_GPU=nvidia` (copy path) | 0 fps, 0.0% | 60 fps, 7.3% |

So the copy path holds the panel's 60 Hz from either NVIDIA GPU; on
schenker it costs ten points of a core more than the display path.

**NVIDIA's own driver takes neither path.** On schenker booted on
NVIDIA's legacy driver (580.178.04, closed kernel module, the GTX 1050
Ti), the KMS round with `FNL_GPU=nvidia` (2026-10-19) never reaches that
GPU. It refuses eDP-1 as NVK does -- `vkGetDrmDisplayEXT` returns
`VK_ERROR_UNKNOWN`, the panel and HDMI being the Intel card's -- and it
is not among the devices that present to the copy path's headless
surface, which are the HD 630 and llvmpipe alone ("no GPU named
"nvidia" among the 2 that can present to this surface"). So the name
chooses nothing, the program is on the HD 630, and the round stops for
the wrong device. The same boot's Xorg and weston rounds are on the GTX
1050 Ti through PRIME offload with nothing configured: 68 checks and 47,
none failed. A KMS frame from NVIDIA's driver on a GPU with no output of
its own wants a third way onto the output -- an image the display's
device imports, or a readback with no surface -- which nothing here has.

**A suspend with a GPU, on aspire.** schenker hangs in suspend to RAM,
so the resume went to aspire's stick (HD 530 beside a GTX 960M, NVK
GM107): 30 s of deep sleep by the RTC (the boot images' tooling), the
program started detached, since the harness's ssh session does not
survive a sleep (*A suspend needs nothing here, on lavapipe*). Each of the four
places, `fenestal-pace --draw`:

| | across the sleep | after |
|---|---|---|
| KMS, `FNL_GPU=intel` (its display) | `surface lost -- rebuilding`, rebuilt on the HD 530 | 60 fps; the same again on a second sleep |
| KMS, `FNL_GPU=nvidia` (copy path) | nothing logged | 60 fps |
| weston, `FNL_GPU=intel` | nothing logged | 60 fps; the same again on a second sleep |
| weston, `FNL_GPU=nvidia` | nothing logged | 60 fps |

No device lost on either GPU. The testbed on KMS, its scene moving (F2
through `uinput_hands.py`), likewise on both: alive after, its frames
on eDP-1 whole and still moving. The display path loses its surface in
the resume and the swapchain rebuild takes it back; the copy
path, presenting to a headless surface, has nothing to lose.

**weston's repaint window, not the client, held it at 30.** With weston's
default `repaint-window` of 7 ms the draw row read 30 fps with
`--frame-times` putting 29.7 ms of each 33 ms frame in the wait for the
frame callback and 3.4 ms in the present. `WAYLAND_DEBUG=client` showed
the client committing 4 ms after each callback and the next callback
29.7 ms after the commit, Mesa's own callback carrying the server's same
timestamp as the library's -- so nothing on the client side waited twice.
weston's GL repaint of a full-window `wl_shm` buffer on Ironlake takes
7 to 8 ms; started 7 ms before the vblank, it misses every one.
`weston-presentation-shm`, whose buffer is small, holds 60 at the
default. By `repaint-window`, `--draw --no-late-latch`:

| `repaint-window` | 7 (weston's) | 8 | 9 | 10 | 12 | 15 |
|---|---|---|---|---|---|---|
| fps | 30 | 56 to 58 | 60 | 60 | 60 | 60 |

The harness's `weston.ini` sets 10 (`_weston_ini()`), the column measuring
the client and not a default sized for faster hardware; a caller on
weston on such a machine gets 30 unless that line is in its config, and
nothing a client does moves it.

**OpenGL draws onto the output's own buffers, and needs no GBM.**
`kms-gl`'s bridge (`src/renderer/gl/egl_kms.c`) opens Mesa's surfaceless EGL
display (`EGL_MESA_platform_surfaceless`), makes the context current on no
surface, and names nothing of DRM. The frame is drawn into the renderer's
own window image as everywhere, then blitted on the GPU into the back
buffer of the backend's scanout pair -- the copy path's two dumb buffers,
which the backend exports as dma-bufs (`kms_scan_export()`) and the
bridge imports once each as EGL images behind a framebuffer -- and the
backend flips it on (`kms_scan_begin()`, `kms_scan_show()`), owning the
output, its mode and its buffers as before. A dumb buffer is linear XRGB,
which a driver that imports a dma-buf takes, so the backend links nothing
and GBM, which would have put libgbm on the link line, is not needed. The
blit is waited for before the flip (`glClientWaitSync`): the kernel need
not wait on the GPU's writes to a dumb buffer.

Where the way is refused -- the kernel will not export, the display has no
`EGL_EXT_image_dma_buf_import`, GL no `GL_OES_EGL_image`, or the GPU
drawing will not make a framebuffer of another device's buffer -- the
frame is read back instead, into a pack buffer ahead of the frame's end
stamp, and handed to the backend's `present_pixels()`, which copies it on
as it does Vulkan's copy path; one line says which way and why
(`FNL_FAULT=kms-gl-copy` forces the readback). The refusal is asked for,
not met: NVIDIA's GL refuses the image behind a renderbuffer
(`GL_INVALID_OPERATION`, "EGLImage not supported"), and a Debug tree's
debug output is kept quiet across that one call (`debug_quiet`), or its
line is the round's failed clean log. The flip is the throttle
either way (`page flip (vsync)` on the status line, and `page flip
(async)` without vsync); the surface has no interval to give. A mode
change needs nothing of the bridge: the backend's pair follows the mode,
and a new pair is imported by its key (`KmsScanBuf::key`).

Measured on schenker's console, the HD 630 driving its panel: flat out,
796 frames a second against 162 by the pack buffer and 102 by
`glReadPixels` into client memory, the GPU's span 1.18 ms; at 60 Hz the
present's CPU time 0.35 ms against 3.6. At 60 Hz the GPU's span reads
7.8 ms, Intel's GPU clocking itself down to a load this light, which the
ranking, measuring at the display's rate, reads as it is. This desktop's
console, the RX 590 on DP-1 at 3840x2160, the case that asked for it:
60 frames a second where the readback held OpenGL at 30, the present's
CPU 0.34 ms against 16.3, the GPU's span 1.6 ms, and 448 a second flat
out; the ranking there now Vulkan 1579, OpenGL 622, where it had put
OpenGL first and drawn at half Vulkan's rate. aspire's HD 530
the same: 881 frames a second flat out, the present's CPU 0.40 ms at
60 Hz. thinkpad's llvmpipe takes its buffers too: the present's CPU
0.17 ms against 3.4, the frame still at 30. schenker's GTX 1050 Ti on
NVIDIA's EGL imports the Intel GPU's buffer and will not put a
renderbuffer over it (`GL_INVALID_OPERATION`), and copies. A texture over
it was tried and is not kept: NVIDIA's EGL made a complete framebuffer of
it, drew into a copy of its own -- the panel stayed black -- and crashed
inside `eglDestroyImageKHR` when the image was let go, a `glFinish()`
before it or not. Since then every buffer imported is checked once: a
colour cleared into it on the GPU and read back through the backend's
mapping at both corners (`scan_lands()`, `kms_scan_peek()`), and a buffer
the GPU's writes do not reach sends the frames the copy path's way. The
pictures, read off the scanned-out framebuffer by `kms_grab.py`, are the
copy path's, row for row; a moving edge broken in steps in them is the
grab's own long read racing the
flips, and shows on Vulkan's copy path the same way. The rounds:
`kms-vulkan-bare@schenker:nvidia`, which walks every entry of the GPU box
and both outputs, 161 checks and none failed, the HD 630's and llvmpipe's
OpenGL onto the buffers and the GTX 1050 Ti's copying; `kms-gl-bare` on
thinkpad's llvmpipe, 55, the one failure the new line in the log, which
the round now expects.

Measured on the three laptops' live keys: aspire's and schenker's EGL was
NVIDIA's -- glvnd lists its vendor first, and its EGL has the surfaceless
platform too -- so the GTX 960M and 1050 Ti draw and the backend flips
their pixels onto the Intel GPU's panel, 43 checks each with `--capture`
reading the scanned-out framebuffer, none failed; the thinkpad, which has
no 4.5, on llvmpipe under `LIBGL_ALWAYS_SOFTWARE=1`, 46. Under `asan`
and `gc` the two NVIDIA laptops are 39 each and the thinkpad 42, none
failed, and under `tsan` the thinkpad 42 -- the NVIDIA ones are skipped,
TSan not following NVIDIA's own driver. schenker's `asan` round once
reported NVIDIA's EGL's D-Bus connection as a leak at exit, the one
`lsan.supp` already names through its GLX library, and the file names
`libEGL_nvidia.so` for it too.

### Win32

`src/backend/win32/win32.c` makes one claim and marks its exceptions: it is
cross-compiled with mingw-w64 and **driven three ways** through the same
harness — under wine, on XWayland against the same RADV the native build uses;
on Windows 10 and 11 in VirtualBox guests, where the frame is lavapipe's (see
*Driving a window*); and on Windows laptops with GPUs of their own (*Measured
on Windows itself*). Wine is not Windows and the code says so where they are
known to differ:

* `WM_NCCALCSIZE`'s wParam: under wine only the FALSE spelling ever arrives.
* **The maximized overhang.** Wine takes `WM_GETMINMAXINFO`'s answer and the
  maximized window is the work area. Windows reads a size that fills the work
  area as "maximized" and puts the frame's overhang back — the window came out
  1515x1098 at (-8, -8) on a 1499x1082 work area — so `max_client_rect()`
  makes the client area the part of that rectangle on the work area and leaves
  the rest off the monitor.
* **A minimized window's size.** Under wine its client area is nothing; on
  Windows it is the stub of a title Windows no longer draws, 160 pixels wide,
  which was published as a resize to 160x1 and back. `publish_size()` asks
  `IsIconic()`.
* Print Screen arrives as a release with no press on Windows, and not at all
  under wine.
* **A timed wait.** Wine honours a timeout to well under a millisecond.
  Windows expires one on the scheduler's tick, 15.6 ms unless the process
  raises it, which made every late-latch hold a missed cycle more often than
  not. Measured in the guest, 150 waits each: an 11 ms timeout on a
  `CONDITION_VARIABLE` came back after anything from 6.7 to 30.7 ms, 13.4 on
  average, and a 1 ms one after 14.7; the same two on the timer below took
  11.9 and 1.5, never more than 2.3 over. Under wine the old wait is 0.1 ms
  late and the new one 0.25. `FnlCond` is therefore an event beside a
  high-resolution waitable timer (`src/core/thread.h`) and not a
  `CONDITION_VARIABLE`: the timer is kernel32's, where `timeBeginPeriod` would
  have been winmm on the link line. Before Windows 10 1803 there is no such
  timer and the wait is the tick's; the overrun rule in *Late latching* is what
  keeps the hold off there. A Windows 11 guest says the same
  (`tools/probe/waittest.c`). The timer was created, and by mean and worst,
  in ms:

  | asked | `CONDITION_VARIABLE` | event + that timer | event + a plain timer |
  |---|---|---|---|
  | 1.0 | 11.70, 16.50 | 1.29, 2.38 | 11.79, 16.22 |
  | 4.0 | 11.81, 16.31 | 4.29, 5.20 | 11.75, 16.29 |
  | 11.0 | 16.39, 26.12 | 11.46, 12.53 | 16.58, 26.07 |
  | 12.5 | 19.80, 28.48 | 12.91, 14.38 | 19.57, 28.48 |

  And on hardware, which is the same answer with the guest's noise taken
  off it. Two laptops on Windows 11 26H2, mains, the timer created on
  both; the first laptop's figures, the second's within 0.05 of every
  mean:

  | asked | `CONDITION_VARIABLE` | event + that timer | event + a plain timer |
  |---|---|---|---|
  | 1.0 | 11.81, 16.15 | 1.19, 1.68 | 11.85, 16.31 |
  | 4.0 | 11.79, 16.25 | 4.21, 4.67 | 11.82, 16.30 |
  | 11.0 | 14.19, 25.63 | 11.25, 11.68 | 13.92, 25.65 |
  | 12.5 | 14.40, 27.67 | 12.74, 13.18 | 15.08, 28.14 |

  A quarter of a millisecond late and never 0.7, where the guest's worst
  was 1.9: the hold's lateness on Windows is the tick's or nothing.

  Never more than 1.9 over on the timer, and a plain waitable timer is the
  tick's as the condition variable is: it is the flag that buys the wait.

A captured pointer turns the view only from a device that reports relative
motion; a tablet or a virtual machine's pointer integration reports positions,
which raw input hands over as such and this backend does not use — measured in
the guest, where absolute reports moved a captured pointer not at all.

The window class asks for no redraw-on-resize style and no private device
context: nothing here paints through GDI, since the swapchain is the window's
content.

The output is published when the window lands on a different monitor, which
costs one `MonitorFromWindow` per step of a drag, and re-read under the same
handle only for what moves its numbers: a mode change (`WM_DISPLAYCHANGE`), a
work area that moved (`WM_SETTINGCHANGE` with `SPI_SETWORKAREA` — a taskbar
resized or sent to another edge), and a change of scale.

Win32 delivers pointer deltas in pixels from raw input. Under wine that is
driven like the other two: input injected through the portal reaches wine as
real pointer motion, and the round's captured-motion check reads back the
delta it sent. XTEST motion reaches wine's raw input only once wine has
clipped the pointer, which it does only for a window with the X focus, and
then a warp arrives as its position (*Wine on the bare machine*).

**The scale is resolved once**, in `resolve_scale120()`, and every place that
wants it asks: the size the window is created at, the size published on every
`WM_SIZE`, and `update_scale()`. Three copies of that one line are three places
for `--scale` to reach or miss, and missing the first opens the window at the
monitor's density rather than the one asked for.

**`App::req` is spent from `FNL_WM_WAKEUP`, not only from the pump's turn.**
`fnl_run()` calls `app_drain_requests()` after `BACKEND(a)->pump()` returns,
which on the other backends is once per turn. Here it can be never:
`DefWindowProc` runs an inner message loop for a titlebar drag or a border
resize, so `DispatchMessageW` does not come back until the button does, and
every `[any]`-thread knob waits with it — `fnl_set_fullscreen()`,
`fnl_request_size_pt()`, `fnl_release_pointer()`, `fnl_titlebar_custom()` and
the rest. Measured with a caller whose own thread asks for 700×500 mid-drag: the
window kept its size for the 8.3 s the drag lasted and took it on release. A
*posted* message is dispatched by that inner loop, so answering the wakeup there
is what puts the deferral back to one turn.

What it does not reach is `fnl_quit()`: `should_close` is tested by
`fnl_run()`'s own loop, and ending an inner one needs it to give up rather than
a request spent. Both documented ways to make it were tried —
`PostQuitMessage(0)` and `SendMessageW(WM_CANCELMODE)` — and neither ends the
loop under wine, measured each time. So a quit asked for mid-drag lands when the
button comes up. That is a stated limitation rather than code that does nothing
on the one system this can be tested on.

**A window nobody can see is parked, whoever says so.** The loop asks the
backend (minimized, hidden) and the renderer (a present that would be swallowed:
Direct3D 12's `DXGI_STATUS_OCCLUDED`). The backend's wait parked only what the
backend knew of, and Win32's answers at once for a window it counts as shown,
so a renderer's "occluded" with vsync off drew flat out, its presents returning
at once. The loop now takes the same bounded wait itself (`app_park()`,
`FRAME_WAIT_S`) when the renderer alone says so and no rebuild is owed -- a size
is still answered at once -- and the skip after it draws nothing while it stays
so. DXGI's "occluded" cannot be brought about on demand: a window covered by a
maximised one on schenker's Windows 11 went on presenting at 950 fps, and a
display turned off by `SC_MONITORPOWER` changed nothing that could be told from
here. So `FNL_FAULT=occluded` forces the renderer's answer, on any platform,
sampled once a turn: `fenestal-pace 0 6 --settle 1 --draw --no-vsync` on this
desktop's X11 Debug tree, 9,300 fps and 7.3 s of CPU in its 7 s as it is,
nothing drawn and 0.08 s under the fault.

### Wine on the bare machine

The Windows tree runs under wine on the laptops' bare setups too, with Vulkan
and with Direct3D 12 on each of three servers (`tools/drive/README.md` *The
Windows tree there, under wine*): on Xorg with no window manager, on weston with
no shell, and on the display with no window server -- which wine cannot do,
since its display drivers are X11's and Wayland's and nothing drives a DRM node.
Its KMS round therefore puts cage between, a kiosk compositor that takes the
node and shows one window, wine drawing through its Wayland driver. cage over
gamescope, which would put XWayland in between as the Steam Deck does: gamescope
wants a GPU whose Vulkan driver sees the DRM node, which lavapipe on the
thinkpad is not, and cage draws with pixman where nothing better is there. Wine
11.20 and cage 0.3.1, Arch's, in each case.

What wine does with no desktop around it, measured there, and what was
done about each:

* **It leaves a window's size to a manager.** By default wine asks the
  window manager to maximize and move a window, and on Xorg with none the
  testbed was told 1920x1046 while its X window stayed 1280x754, and every
  section after it read the wrong window. `Managed=N`, wine's own setting
  for an X server with no manager, has wine place, size and maximize its
  windows itself, as override-redirect ones; the harness writes it into
  the prefix. And wine reads no position back off the server: the window
  moved by a `ConfigureWindow`, as the X11 round centres its own, was one
  wine still thought was at (0, 0), and every click landed 320 pixels off
  -- so wine's window is moved only by its titlebar.
* **It never takes the X focus.** With `Managed=N` wine sets the focus on a
  window only where a child of it has the Win32 focus (`X11DRV_MapNotify`
  asks `IsChild`), so a top-level window never has it, on a fresh server
  as on one up for hours. Keys reach it through PointerRoot; but wine
  clips the pointer only for a process that has the X focus
  (`is_current_process_focused()`), and without the clip it reads no raw
  motion, so a captured pointer neither stayed in the window nor turned
  the view. The round gives the window the focus at start, as a click
  would under a manager. Raw motion then arrives, from XTEST too, and
  XTEST's warps arrive as their position (a "delta" of 943, 604): the
  check is that relative motion arrives, which it now does.
* **A fullscreen that changes no size.** With no panel the work area is
  the monitor, so the maximized window already had fullscreen's rectangle;
  the `SetWindowPos` changed nothing, Windows sent no `WM_SIZE`, and the
  library, which publishes the window's state with a size, never said it
  was fullscreen. The same holds on Windows on a monitor without the
  taskbar, so `win32_set_fullscreen()` now runs the size handler itself
  when its move produced none.
* **Its helpers outlive the program.** wineserver, `services.exe`,
  `explorer.exe` and five more are handed the program's stderr, and the
  ssh that carries it ended 4.4 s after the program, past the round's
  three-second wait for Escape. The shell there waits for the program
  instead and says when it has gone and with what code.
* **It composes the dead keys itself**, on X through Xlib's Compose table:
  an accent and a space is an apostrophe, and an accent before a letter it
  does not go on is nothing. The library's rules are not asked there
  (`COMPOSED_ELSEWHERE`). Its Wayland driver types through a table of its
  own: under German, Y is y and the o-umlaut key a u-umlaut, so neither
  Wayland round declares `known-layout` -- `win64-vulkan-wl` does not
  declare `desk-layout` for the same reason.
* **Fullscreen back to maximized on weston** is lost. Leaving fullscreen
  the library restores the window and then maximizes it, and wine sends
  `unset_fullscreen` and then `set_maximized`; weston answers the first
  with a configure of its own, which wine takes for its answer to both
  (`WINEDEBUG=+waylanddrv`): it clears `WS_MAXIMIZE`, calls a window that
  covers the monitor and is not maximized fullscreen, and weston is left
  showing a floating window as wide as the monitor at the restored
  window's place, its buttons off the screen. mutter answers the two with
  one configure, and the rounds on this desktop pass. The round skips
  that leg there (`MAXIMIZED_LOST_IN_FULLSCREEN`); leaving fullscreen
  straight into maximized would avoid it, at the cost of a change on
  Windows itself that only the guests and the laptops booted into Windows
  can check.
* **weston aborted once**, on schenker, as the Vulkan program under wine
  exited after a round it had passed: its own assertion in
  `weston_surface_attach()` (`surface-state.c:241`, weston 15.0.1), on a
  buffer whose size it could not divide by the surface's scale -- a
  client's mistake weston answers by dying where a protocol error was
  due. Not seen again in the rounds since. Its socket outlived it, so the
  harness asks for the unit as well before it takes weston for running.
* **What it costs, by `pace.py`** (2026-11-01, two sweeps, wine 11.21,
  lavapipe; `docs/benchmarks.md`). Vulkan under wine paces as the native
  program does, size rate for size rate -- 19 to 20 a second at 5 sizes,
  45 to 47 at 15, 60 from 30 -- at up to 8 points more of a core on
  Xorg and 7 to 40 more on weston; cage refuses sizes, a kiosk's one
  window being its output, and reads as KMS does. Direct3D 12 on
  vkd3d-proton is bound by the two cores: from 30 sizes a second 46 to
  53 frames at more than a core, a window at rest 11 to 34% where
  Vulkan's is 1, and the two sweeps apart by up to 14 fps on weston,
  so its column says where the cores run out and not a rate. `draw`
  under wine on Xorg presented 70 to 72 a second on the 60 Hz panel,
  where the native program presents 60: wine's present there is not
  held to the refresh, not looked into. The columns' CPU is the
  program's and the wine server's together (`measure_bare()`).
* cage accelerates its pointer and has no setting to stop it, so the
  harness's hands there are an absolute pointer, a tablet over the screen,
  whose counts libinput maps onto it one to one.
* **A wheel step goes missing under wine's Wayland driver**, one pair of
  steps in forty on the thinkpad, on weston and on cage. weston sent
  both (`WAYLAND_DEBUG`); wine's server lost the second. It hands the
  window's thread a `WM_MOUSEWHEEL` and keeps it queued until
  `accept_hardware_message`, and `merge_mousewheel()` adds a step that
  arrives meanwhile to that same message, which the accept then frees
  (`server/queue.c`, wine 11.20, unchanged on master). The Wayland driver
  sends input from a thread of its own, so a step can land in that gap;
  the X11 driver's comes from the window's thread between messages and
  cannot, and two steps that arrive before the program asks are merged
  into one event of -2, correctly. `tools/probe/wheelcount.c` reproduces
  it alone, every pair with a mouse hook of a millisecond holding the gap
  open, and a wineserver patched not to merge into a message it has
  handed out loses none; the report is drafted in
  `docs/wine-wheel-merge.md`. The library steps round it under wine
  (`BackendState::wheel_raw`): raw mouse input is asked for at open, and
  a wheel step is taken from `WM_INPUT`, which wine queues ahead of the
  step's message and never merges, wherever the window has the focus and
  the pointer; the `WM_MOUSEWHEEL` that follows is dropped as its echo,
  and anywhere else -- an inactive window under the pointer -- the
  message is the step, as on Windows, which this changes nothing of. On
  the thinkpad under weston, 120 pairs from `wheelpairs.py`'s timing with
  four busy loops on the CPU: 7 lost before, none after; idle, 1 and
  none; under load on cage and on Xorg, 60 pairs each, none lost and
  none counted twice.

With those, the stress round passes in all six environments of each laptop:
54 checks on Xorg (55 on the thinkpad, whose window is first taken in by
its border), 39 on weston (40) and 36 on cage, none failed, on aspire,
schenker and the thinkpad. Vulkan and Direct3D 12 drew on the GTX 960M
and the GTX 1050 Ti, and the thinkpad on lavapipe. On Xorg wine names a
device after the server's RandR provider and not its driver: `NVIDIA-G0`
on schenker, and `modesetting` for the thinkpad's DXGI adapter.

On the thinkpad a frame at a new size can take longer than `size_await()`
waits for one, a quarter of a second, and the library says so: Direct3D
12 there is vkd3d-proton on llvmpipe on a 2010 CPU, and two of six rounds
on 2026-10-21 said it, one each on Xorg and weston, the other four not.
The window goes on as it should, sizes no longer held, and a round on a
device that is the CPU takes the line as no complaint, for every wine
tree (`_WineOnBare.device_found()`); on a GPU it is still one.

### Wine 11 on this desktop

WineHQ's 11.0 (`wine-stable`) and 11.21 (`wine-devel`), its builds for
Ubuntu's `resolute`, sit under `/opt` beside the distribution's 10.0,
which every wine round here runs unless `FNL_DRIVE_WINE` names another;
each named wine has a prefix of its own (`tools/drive/README.md` *Another
wine than the distribution's*). A wine declaration in the harness holds
for the wines it has been seen to hold for (`WINE_HELD`), and any other
is put to the checks. One round of the four environments under each, the
two Direct3D 12 ones once on vkd3d-proton and once on wine's own vkd3d
(`FNL_DRIVE_VKD3D=wine`), on 2026-11-01:

| checks, failed | 11.0 | 11.21 |
|---|---|---|
| `win64-vulkan` | 92, 2 | 92, 5 |
| `win64-d3d12`, vkd3d-proton | 92, 2 | 92, 5 |
| `win64-d3d12`, wine's vkd3d | 92, 3 | 92, 6 |
| `win64-vulkan-wl` | 43, 8, cut short | 9, 4, cut short |
| `win64-d3d12-wl`, either | 43, 8, cut short | 9, 4, cut short |

* **Wine's own vkd3d draws the tree's DXIL** in both: the X11 round's GPU
  box moved `win64-vulkan` to Direct3D 12 on it, and the Direct3D 12
  rounds on it drew from the start. What 10.0 refuses is 10.0's alone.
  Its DXGI has no frame statistics -- `GetFrameStatistics` answers
  `E_NOTIMPL` -- and the library says so once a swapchain and goes on
  without the wait for a size to be shown (`wait_shown()`), which a round
  on wine's vkd3d takes as no complaint (`WINE_DXGI_NO_STATS`).
* **11.21 hands a window each key twice where an input method has been
  at it**, and 11.0 does not. `tools/probe/keytwice.c`, a window that
  prints its keyboard messages, typed into through the portal by
  `keytwice.py` beside it: under the Wayland driver every key comes as
  `VK_PROCESSKEY` (0xE5) with the key's scan code and then as the key,
  down and up alike; under the X11 driver each key comes once until the
  first key XIM filters -- the dead acute under German -- and as under
  Wayland after it, the layout switched back or not. The probe's own
  composition handling, the default or `win32.c`'s, changes nothing. The
  library takes a key by its scan code (`key_from_scancode()`), so the
  two are one key pressed twice, and every toggle in the round undoes
  itself: text mode stays on, the scene never animates and rests at two
  frames a second, Escape quits nothing, and on
  Wayland F11 leaves fullscreen as it enters it, which ends that round.
  Nothing is done about it here: wine's, and the library's answer on
  Windows, where an input method's `VK_PROCESSKEY` stands in place of
  the key and not beside it, is right as it is.
* **11.0's X11 driver has no clipboard to cross**: a copy did not reach
  `xclip`, and a paste gave the window its own text back. 10.0 and 11.21
  pass both.
* **11.0's Wayland driver** opens the window where 10.0's did, on DVI-D-1
  with the pointer on DP-1, and lets the compositor's resize take it to
  119 points under the 625 floor, as 10.0's did: both declarations hold
  for 11.0. It now says the window changed monitor, which 10.0's never
  did, `DISPLAY1` to `DISPLAY2`; but the output size it gives is no
  monitor's -- 1920x1041 points before the move, 1920x2160 on DVI-D-1's
  1920x1200 after it -- and the move back leaves it at `DISPLAY2`. After
  that the titlebar controls' press reached nothing, and the round lost
  the window before the typing.

### Measured on Windows itself

Wine is not Windows, so the Win32 build is also driven in a VirtualBox guest
and on two Windows laptops with GPUs of their own (`tools/drive/README.md`
*Driving a window* is how). What follows is what that found about the
library.

**What a size request costs on Windows is the driver's two calls.** Timers
around each step of a rebuild, the mean of thirty at 30 sizes a second, in
milliseconds:

| | `vkCreateSwapchainKHR` | `vkDestroySwapchainKHR` | the drain before it |
|---|---|---|---|
| HD 630, first laptop | 15.6–17.0 | 9.5–10.7 | 1.8–2.4 |
| HD 530, second | 12.9–13.5 | 7.5–7.9 | 4.4–5.2 |
| GTX 1050 Ti, first | 17.3–18.2 | 0.1 | 0.3 |
| GTX 960M, second | 1.4–1.5 | 0.1–0.3 | 1.1 |

against 3.6 and 0.39 for the create on X11 and Wayland here. The surface
query, the views, the depth image and the framebuffers are under a tenth
of a millisecond together on all four, a millisecond for the depth image on
the first NVIDIA device. So on Intel a new size is 25 to 30 ms inside the
driver and the frame rate under a resize is one over that, which is the 34
and 42 fps at thirty sizes a second; and nothing this library does between
the two calls is in it. Three things were tried:

- **the old swapchain destroyed before the new one is made**, with no
  `oldSwapchain` handed over: 13, 21, 0 and 27 fps on the four devices at
  thirty sizes a second, the create rising to 46 ms on the first NVIDIA
  one. Retiring is the cheap order and stays;
- **one image more than the driver's minimum**, which is 2 on all four:
  the same figures to the millisecond. And two images under FIFO is 60 fps
  idle on all four, which was the open question behind it;
- **the fence wait in the period estimate** (*The period is measured*,
  above), which is what the first laptop's NVIDIA column wanted.

After that last, a full sweep of each:

| sizes a second | HD 630 | GTX 1050 Ti | HD 530 | GTX 960M |
|---|---|---|---|---|
| idle | 60 fps, 3.4% | 60, 6.2% | 60, 2.6% | 60, 5.7% |
| 5 | 56, 14.1% | 60, 10.4% | 60, 10.1% | 60, 9.1% |
| 15 | 44, 34.9% | 55, 20.7% | 54, 33.1% | 60, 15.3% |
| 30 | 36, 65.1% | 56, 31.2% | 42, 56.5% | 54, 30.2% |
| 45 | 30, 76.0% | 40, 47.0% | 36, 77.8% | 59, 44.5% |
| 60 | 26, 85.4% | 34, 59.6% | 30, 78.6% | 54, 58.5% |
| 250 | 24, 83.8% | 3, 82.5% | 28, 84.0% | 10, 59.3% |

Nothing runs past the panel any more, and each column is its device's
create and destroy: the GTX 960M, whose create is a millisecond and a half,
is the one that holds. The four local environments are what they were, 59 to
61 in every cell. The bottom row is another thing. At 250 sizes a second
both NVIDIA devices turn the loop at its usual 55 to 60 a second and
present 3 and 10 of them: the window has a newer size by the time the
frame built for the last one reaches the present. That reading is the
frame lines' — the turns are there and the presents are not — and has not
been confirmed at the call; the Intel devices, which present what they
have, are at 24 and 28. `TODO.md` has it. The round on that build, with
captures: Wayland 56, X11 74, wine 65, wine on Wayland 41, the two guests
69 each and the two laptops 66 each in one bare round, none failed;
AddressSanitizer and ThreadSanitizer on the two native targets, 53 and 74
each; and validation with sync and best-practices on those two and both
laptops, 53, 74, 63 and 63, the layer loaded on each laptop.

**One size at a time, on Win32.** The bottom row was what it looked like.
The window procedure now holds a size until the frame drawn at it has been
presented (`size_await()` in `win32.c`, a quarter of a second at most): a
size is published with a serial, as a configure is on Wayland, the frame
that answers it acknowledges it through the seam's `ack_configure` and
`flush`, and the procedure waits for that, dispatching what other threads
send it meanwhile. Sizes that arrive faster than the driver rebuilds are
then never the window's, every rebuild ends in a present, and a caller's
requests coalesce in `App::req` while the last is drawn — which is also why
wine's 250 row costs 20% of a core now and cost 36. A wait that times out
says so in the log and is the last until a frame has answered a size
again, so a window that is not drawing is not a window that stutters.

**And the retired swapchain is destroyed on a thread of its own, where
that is slow** (*the reaper*, `swapchain.c`): started the first time a
`vkDestroySwapchainKHR` is measured over 2 ms, which is the two Intel
devices and nothing else here. Vulkan lets it run beside the next
`vkCreateSwapchainKHR`, and Intel's driver does not survive that — four
runs of twelve at 250 sizes a second ended in `0xC0000374` or
`0xC0000005` inside it, none of fourteen with the two calls kept apart
(`RendererState::reap_serial`). Kept apart, what is gained is the destroy done
while a frame is drawn: the middle of the Intel columns and not their end.

| sizes a second | HD 630 | GTX 1050 Ti | HD 530 | GTX 960M |
|---|---|---|---|---|
| idle | 60 fps, 2.9% | 60, 6.5% | 60, 2.6% | 60, 5.5% |
| 5 | 57, 14.8% | 60, 15.8% | 60, 13.8% | 60, 9.9% |
| 15 | 46, 34.6% | 57, 20.8% | 58, 35.6% | 59, 17.2% |
| 30 | 40, 68.2% | 55, 34.6% | 52, 57.0% | 53, 30.7% |
| 45 | 34, 74.4% | 50, 50.2% | 41, 81.2% | 60, 41.1% |
| 60 | 35, 72.9% | 46, 53.6% | 39, 83.0% | 60, 60.6% |
| 250 | 35, 72.9% | 49, 51.5% | 41, 80.4% | 60, 60.8% |

Against the table before it the bottom row is 24, 3, 28 and 10, and the
sixty row 26, 34, 30 and 54. What is left under 60 is the driver's create,
one a frame. The four local environments read 59 to 62 in every cell, as
before. On that build: the bare round with captures, Wayland 56, X11 74,
wine 65, wine on Wayland 41, the two guests 69 each, the two laptops 66
each, and the laptops again on their Intel devices, 66 each; `minimal.py`
on all eight, 19 each; AddressSanitizer and ThreadSanitizer on the two
native targets, 53 and 74; validation with sync and best-practices on both
laptops on each device, 63 four times with the layer loaded — which on the
Intel devices is the layer's thread checks watching the reaper. None
failed.

**Direct3D 12 is the comparison, and the extension that would avoid the
create is not on Intel.** The same sweep on the `win64-d3d12` tree,
schenker's HD 630 at sixty sizes a second: Vulkan `rebuild 26.4` ms a
frame and 36 fps, Direct3D 12 `rebuild 18.3` and 50 -- `ResizeBuffers`
resizes the swapchain in place and is slow on Intel too, only less so;
every other phase is under 0.2 ms on both. Vulkan on Win32 has no
in-place resize: a surface's extent is its window's, so a new size is a
new swapchain. `VK_EXT_swapchain_maintenance1` with
`VK_EXT_surface_maintenance1` would let one swapchain present into a
window of another size, one to one at a gravity, and skip the create
through a drag; `vulkaninfo` on schenker has NVIDIA's driver offering
both (one-to-one, every gravity) and Intel's, 101.2145, offering the
surface half with no scaling at all (`supportedPresentScaling` none) and
not the swapchain half. X11 has no DXGI, so there the same extension is
used where the device offers one to one and minimum gravity on both
axes (*A swapchain larger than the window*). Win32 does not take it.
So on Intel the create is the floor, and on NVIDIA only the GTX 1050
Ti, whose create is 17 ms, would gain -- the
960M's is 1.5. Skipping sizes during a drag instead would bring back the
frames drawn at the wrong size that *One size at a time* removed.

**A size is on the screen before its frame, whatever the API.** Windows
sends `WM_SIZE` after it has applied the size, so the desktop composes the
window at the new size with the old frame -- cut short when the window
shrank, beside black or the desktop when it grew -- until the frame drawn
for it is shown. `size_await()` holds the *next* size; it cannot hold the
one it is answering, and a Vulkan surface on Win32 takes no extent but
its window's, so no frame can be ready first. What is left to choose is
how long each size shows the wrong frame. Counted on schenker by
recording the desktop through Desktop Duplication (ffmpeg's `ddagrab`,
run on the laptop's desktop by the agent) while the agent drags the
corner 40 pixels a step at 60 Hz, in and out twice, and reading each
frame's colour bars against the window's edge -- frames of another size
than the window, of some 45, and the drag's frame rate:

| | GTX 1050 Ti | | HD 630 | |
|---|---|---|---|---|
| Vulkan | 24 | 26 fps, rebuild 22 ms | 35 | 12 fps, rebuild 82 ms |
| Direct3D 12, ack at `Present` | 41 | 39 fps | 39 | 35 fps |
| Direct3D 12, `DwmFlush` first | 28, 35, 32 | 26 to 29 | 25, 23, 25 | 23 to 26 |
| Direct3D 12, ack when shown | 15 to 25 | 15 to 16 | 19 to 21 | 15 to 18 |

The run-to-run drift is as large as some of those differences -- the
same build let through 15 three times in one hour and 21 and 25 the
next -- so what decided between two waits was three rounds of each,
interleaved: on the GTX 1050 Ti, 44, 43 and 42 with no wait, 27, 22 and
23 waiting until shown a vertical blank at a time, and 30, 30 and 31
polling the same statistics every half millisecond.

Direct3D 12's `Present` returns with the frame queued, and acked then the
size was released before the frame was on the screen, so the window ran
ahead of its frames: the faster resize made more frames of the wrong
size, not fewer. So the frame that acks a size now waits until the
swapchain's frame statistics count it shown (`wait_shown()` in
`d3d12/frame.c`), as Vulkan's waits on `VK_KHR_present_wait`. They report
it on the third vertical blank after the present, 36 to 49 ms, which
the drag paid in frames while the render thread made the wait: the border
row read 17 and 16 fps on schenker's two GPUs, 19 and 15 on aspire's. It
is the shown waiter's now, as on X11 (*X11: xcb, not Xlib*), and the
size still waits for its frame (`size_await()`): 50 and 50, 56 and 49
(2026-10-29). `pace.py`'s rates say the same, counted (*At rest*): from
30 sizes a second schenker's GTX 1050 Ti drew 16 fps at `8220d5a`, the
wait on the render thread, and 53 at `a24e42f`, its HD 630 45 and
aspire's two 50 and 60; a bisect puts the 16 at `a8904e3`, the wait's
own commit, against 53 just before it. Waited a blank at a time, which releases
the size just after one; polled instead, at half a millisecond on a
high-resolution timer or at `Sleep(1)`, it let through more, so the
moment of the release counts and not only its delay. `WaitForVBlank`
takes no timeout, so the time is read after each blank, and the wait
ends at three blanks or 60 ms; a swapchain that gives no statistics is
not asked again until it is made anew. Under wine the statistics are
polled at `Sleep(1)` within the same 60 ms instead: DXVK's
`WaitForVBlank` (3.1.1, the DXGI the harness runs vkd3d-proton with)
waits for no blank but for one estimated from the refresh rate, and
spins on the clock through the end of it. A 12 s drag of the testbed's
corner on this desk, one each way, the program's CPU and frames:
under wine on X11 39% of a core and 53 fps (lowest second 43) waiting
on the blank, 15% and 58 (51) polling, `dxgi.dll` going from 21% of
perf's samples to 0.6%; on wine's Wayland driver 51% and 33 (26)
against 29% and 41 (35). Vulkan under wine on X11 was 13% in the same
drag. Two cheaper signals were tried
and left out. The frame-latency object was already signalled every time
-- its count runs ahead after the waits a size skips -- so it waited 0 ms
and changed nothing. `DwmFlush` (from `dwmapi.dll`, loaded at run time,
so off the link line) returned in 5 to 25 ms and removed fewer: it
returns when the desktop has composed, not when it has composed this
frame.

Vulkan's rebuild on the HD 630, 82 ms a frame through this drag against
26 at sixty sizes a second in the sweep above, is the testbed's
`NvOptimusEnablement` export and not the drag. Timed inside the rebuild,
`vkCreateSwapchainKHR` alone is 50 to 80 ms there, the queue's drain
before a retire 8 to 18, and the reaper's lock up to 22; in the same
drag `fenestal-minimal`, which exports nothing, creates in 17 to 22 ms,
as `fenestal-pace` does at sixty sizes a second. The testbed built with
the export at 0, interleaved with it as built, two rounds each: the
median create 27 and 28 ms against 62 and 60, the drag 18 and 19 fps
against 11 and 11. So a program that asks NVIDIA's driver for the
discrete device and is then put on the Intel one, by `FNL_GPU` or the
caller, pays twice the create on every size; Direct3D 12's resize in
the same drag, export and all, was 26 ms.

**NVIDIA's MAILBOX on schenker is two frames a refresh.** Vsync off,
`fenestal-pace --draw --no-vsync` on the GTX 1050 Ti, driver 582.78:

| | fps | frame | where it goes |
|---|---|---|---|
| Vulkan, MAILBOX (vsync off's choice) | 116 to 122 | 8.3 ms | acquire and present, `fence` 0.01 |
| Vulkan, IMMEDIATE (a build preferring it, once) | 481 to 490 | 2.1 ms | `fence` 0.85, the GPU busy |
| Direct3D 12, sync interval 0 | 889 to 906 | 1.1 ms | `fence` 0.3 to 0.6 |

The MAILBOX frame is half the panel's period and the time moves between
the acquire (1.9 to 7.7 ms) and the present (0.6 to 6.1) while their sum
holds, so the driver's presentation paces it, not the GPU: the same card
draws 490 a second under IMMEDIATE and 900 under Direct3D 12. The surface
offers FIFO, FIFO_RELAXED, MAILBOX, IMMEDIATE and FIFO_LATEST_READY, and
vsync off takes MAILBOX where it is offered, because it does not tear
(`choose_present_modes()`). aspire's GTX 960M on the same driver drew 683
under MAILBOX, so the cap is not the driver's everywhere (`TODO.md`).

Not NVIDIA's present-method setting either (2026-11-01, both laptops,
driver 582.78, branch r582_73, read through NVAPI's driver settings).
The setting, `0x20D690F8`, which the driver names "Vulkan/OpenGL present
method" and lists with the values 0, 1 and 2, its default 2 (Auto; 0 is
native and 1 layered on DXGI, by NVIDIA Profile Inspector's users), is
unset on both, in the base and the global profile, and neither program
has a profile of its own, so both run Auto. Written into schenker's
global profile, 0 and then 1, MAILBOX drew 120.7 a second each time, as
unset (120.6), and the setting was deleted after. The same day, flat out
on each GPU's own swapchain:

| | GTX 1050 Ti (schenker) | GTX 960M (aspire) |
|---|---|---|
| Vulkan, MAILBOX | 120.6, 120.7 | 790.8, 793.5, the acquire 0.01 ms |
| Vulkan, IMMEDIATE (a build preferring it) | 501, 552 | 1,322, 1,337 |
| Direct3D 12, sync interval 0 | 863 | 1,368 |

So the same driver with the same setting paces MAILBOX at two frames a
refresh on the Pascal card and not on the Maxwell one: the hardware, or
what the driver does with it, and nothing a program or a profile chose.
Temperatures through these runs, every 2 s: the board's sensor 66 °C
idle and 78 at most, the GPU 53 to 60 at up to 96% load.

**A second monitor on the first laptop**, a Samsung at 1920x1080 on HDMI
to the right of the panel, at 100% where the panel is at 125%. The move
between monitors, which every Windows round had skipped, runs: 69 checks
and none skipped, the window reporting `DISPLAY2` and a room of 1920x1032
points there against 1536x816 at home, so the move is a change of scale
as well. `FNL_DRIVE_WIN_OUTPUT=DISPLAY2` runs a Windows round, or a
pacing sweep, with the window on that monitor of that machine from the
start, and there the round is 69 again on each device, validation with
sync and best-practices 66 on each with the layer loaded, and
`minimal.py` 19. The sweep on it, frames and CPU of a core:

| sizes a second | HD 630 | GTX 1050 Ti |
|---|---|---|
| idle | 60 fps, 3.1% | 60, 6.0% |
| 5 | 60, 12.8% | 59, 11.4% |
| 15 | 60, 29.7% | 60, 19.3% |
| 30 | 56, 52.8% | 56, 29.4% |
| 45 | 45, 81.5% | 51, 53.3% |
| 60 | 46, 82.3% | 52, 58.6% |
| 250 | 46, 79.9% | 56, 64.3% |

Better than the panel's columns at every rate that resizes, on one
machine and one driver: at 100% the same window is 1280 pixels wide and
not 1600, and what a swapchain costs to make goes with its size. That is
most of what stood between this laptop and the second, whose panel is at
100%.

**And that monitor at another rate than the panel's.** It offers 23 to 60 Hz
at its size (`tools/probe/modes.c` lists a desktop's displays, their adapter
and their rates, and puts one at a rate for the session); both it and the
panel are the Intel device's outputs. At 50 Hz beside the panel's 60 the
round holds on each device, started on the panel and moved across and
started on the monitor: 66 four times, none failed. The sweep on it:

| sizes a second | HD 630 | GTX 1050 Ti |
|---|---|---|
| idle | 60 fps, 4.4% | 50, 3.4% |
| 5 | 59, 12.8% | 50, 11.5% |
| 15 | 60, 31.2% | 48, 19.8% |
| 30 | 57, 54.4% | 50, 34.4% |
| 45 | 44, 80.4% | 47, 48.9% |
| 60 | 45, 76.0% | 46, 57.3% |
| 250 | 44, 83.0% | 50, 54.7% |

The two devices are held to different clocks. NVIDIA's present waits for the
monitor the window is on -- `frame 20.00 ms`, `wait 19.77`, and at 30 Hz
`frame 33.34 ms` and 30 fps, idle and at thirty sizes a second. Intel's waits
for the panel whichever monitor that is: `frame 16.66 ms` and 60 fps on the
monitor at 50 Hz and again at 30. So on the Intel device a window on a slower
monitor draws frames that monitor has no refresh for, one in six at 50 Hz
and one in two at 30; nothing in the round or the log says so, and what
becomes of them was not looked at. Nothing in the library takes the rate
for 60: the period is the one the present gives, and the spacing and the
late-latch hold follow it on both -- `hold 12.2` on Intel and none on
NVIDIA, whose wait is in the fence, as on the panel. One sweep each at 50
Hz and two cells each at 30.

**The panel at 150%.** A scale is a setting, so the first laptop's was moved
in Settings from 125 and the round run again. It is where the opening
window came to be fitted to the work area (*The presentable area and the
grab ring*): before that the round failed three checks on each device and
stopped at the twenty-second, every one of them a window with its corner
off the screen. Fitted, the window opens at all the room there is, which
leaves a round none: so one that finds less than 300 by 220 points around
its window takes it in by the border first (`ROOM` in `stress.py`), 1280x638
to 980x418 here, and that is a check of its own. 67 on each device, none
failed; and on the build with it the bare round is what it was everywhere
else, Wayland 53, X11 74, wine 65, wine on Wayland 38, the guests 66 each
and the second laptop 63, with `minimal.py` 19 on all eight. The sweep, on
the same panel:

| sizes a second | HD 630 | GTX 1050 Ti |
|---|---|---|
| idle | 60 fps, 3.4% | 60, 8.8% |
| 5 | 55, 14.6% | 58, 10.4% |
| 15 | 43, 41.4% | 56, 22.4% |
| 30 | 27, 62.7% | 53, 35.9% |
| 45 | 25, 71.3% | 52, 41.6% |
| 60 | 25, 64.3% | 41, 46.1% |
| 250 | 26, 72.1% | 42, 46.6% |

Under the 125% columns at every rate that resizes, the Intel one by a third:
the same points are more pixels, and what a swapchain costs to make goes
with its size. One sweep each, before the window was fitted, which a sweep's
own sizes are under.

**And at 175%, the most this panel is offered.** 1097x617 points, and the
window opens 1097x535 presentable and is taken in to 798x316. One check
failed there and was the round's: it dragged the window by the middle of
its bar, which at that width is the controls -- right-aligned, and no
narrower for the window being so -- and a press on them drags nothing. A
window taken in is dragged by the title's end of its bar (`BAR_TITLE_X`),
and the round is 67 on each device, none failed. 200% is not a step
Settings has for it. The sweep there, and a hand on the border:

| sizes a second | HD 630 | GTX 1050 Ti |
|---|---|---|
| idle | 60 fps, 4.2% | 60, 7.3% |
| 5 | 55, 18.0% | 57, 16.1% |
| 15 | 36, 45.8% | 54, 28.9% |
| 30 | 22, 67.7% | 51, 32.5% |
| 45 | 22, 64.8% | 45, 37.2% |
| 60 | 21, 65.6% | 54, 41.9% |
| 250 | 21, 66.4% | 43, 40.6% |
| `border` | 16, 110.0% | 46, 41.9% |

**The scale is set from here, and every step of both panels has had its
round.** `tools/probe/scale.c` reads and writes a display's scale as
Settings does, through `DisplayConfigGetDeviceInfo` with a type that is in
no header; the older `SystemParametersInfo` pair answers on this Windows
and means nothing -- the same -2 at 125% and at 175%, and a set that is
accepted and does not happen. Both panels are offered 100 to 175%. On each
device of each laptop, none failed:

| | 100% | 125% | 150% | 175% |
|---|---|---|---|---|
| first laptop | 66 | 66 | 67 | 67 |
| second | 63 | 64 | 64 | 64 |

The counts differ by the one check a window with no room adds, and the
second laptop's are its own by three throughout. The second's sweep at
175% is the first's over again, 21 fps on the HD 530 from thirty sizes a
second and 43 to 45 on the GTX 960M, `border` 21 and 48: at that scale the
two machines' panels are the same number of pixels to a window, which at
their own settings they were not.

**A suspend, and a laptop that comes back by itself.** `tools/probe/nap.c`
puts a machine to sleep with a waitable timer set to resume it. The first
time, on the first laptop -- S3, wake timers allowed on mains for the
purpose -- the timer woke it a minute later (the system log's wake source:
"possibly due to timer - nap.exe") and two minutes after that it slept
again, "System Idle". A machine a timer wakes is *unattended* until input
arrives: its displays stay off, and Windows puts it back to sleep after the
hidden "System unattended sleep timeout", 120 s on that laptop on mains and
on battery. Holding the system and the display on
(`SetThreadExecutionState`) kept it up and left the displays dark: the
request is honoured, and keeps a display on without turning one on. And
over ssh, in the services session, the display half is accepted and
ignored -- `powercfg /requests` lists the process under SYSTEM alone.

What ends the state is input. `nap` now sends a pointer move of a pixel and
back once it is awake, and is started on the desktop by the agent
(`tools/probe/native_nap.py`, through the agent's `spawn`, which leaves a
program running when the link goes and logs it to a file). With that and no
hold at all, the displays came on with the wake, the machine stayed up past
the two minutes, and the log has no sleep after it. Two things beside it
were the machine's and not the probe's: "Require a password on wakeup" was
on, so every resume stopped at the sign-in screen, which the agent cannot
pass -- now off on mains only; and its firewall drops ping, so only ssh
says whether it is up.

**And a window across it.** `native_nap.py --testbed` has `fenestal-testbed`
drawing, with `--frame-times`, before the machine sleeps, and the agent
asks after the window once it is back. On the first laptop's GTX 1050 Ti,
a minute asleep: the window was there and presenting at 60 fps, 16.66 ms a
frame, from the wake on, with no rebuild, no lost device and nothing in
the log but frame lines; the screens captured with it show the frame
whole. On the second laptop's GTX 960M the same, and its log shows where
the minute went: one present that took it (`present 31112.50`, two frames
in a window of 62 s), one line at 2 fps with a 381 ms wait on the device,
and 60 fps from the next. On the first no frame line spans the minute --
the lines either side read 16.66 ms -- and why the two clocks differ there
is not known. On the two Intel devices (`--gpu intel`) the window was
there and drawing after the wake as well, but at about 200 fps, FIFO and
vsync on, with `pace` and `hold` at zero -- in a window a second old when
the machine slept. Intel's FIFO blocks in the fence wait and nowhere else,
and in those two it did not block after the wake; the spacing had learnt
no period yet, so nothing held the loop. A window left ten seconds first,
on the HD 630: 60 fps before, two seconds of 0 to 900 fps across the
wake, and 60 after with the fence wait at 16.6 ms again -- but the
late-latch hold, 12 ms before, still at zero seventeen seconds on: the
stall across the wake read to the measured controller as holds
lengthening the turns, and each cut its ceiling, which creeps back at 0.3
ms a second. How the program is started is not it: `spawn` and `run`,
alternated four times with no suspend, read the same 60 fps.

**So a resume rebuilds the swapchain and learns the hold again**
(`app_resumed()`, from `WM_POWERBROADCAST` with `PBT_APMRESUMEAUTOMATIC`,
and `frame_resumed()` on the render thread, which logs `back from a
sleep`). The display period is kept: the wake's first turns would teach it
nonsense, and spacing by the old one is safe either way. The window a
second old again, on each Intel device: 944 fps after the wake on the HD
630 and 260 on the HD 530, then the rebuild, and 58 to 60 fps from the
next line with the hold back at 12 ms within a second. On the NVIDIA
devices, which needed none of it, the rebuild costs nothing to see: 60 fps
from the line after it, the GTX 960M's hold back at 12 ms within two
seconds and the GTX 1050 Ti's at none, as it is there. Not `RESUMESUSPEND`,
which comes only once a hand has been on the machine. The Linux backends
are not told of a resume and have needed nothing (*Wayland* and *X11*, on
lavapipe; *KMS: no window server*, on aspire's two GPUs, where the display
path's lost surface is the rebuild it always is). A second monitor, the lid
and a longer sleep are not tried.

**Intel's driver sometimes did not come back from a swapchain call, and
what prevents it is a rest.** About one start in twenty of `fenestal-pace`
on the HD 530 (31.0.101.2111), and once on the HD 630 (31.0.101.2145), the
frame's thread entered `vkCreateSwapchainKHR` or `vkDestroySwapchainKHR`
within the first few resizes of the window and stayed: the window stopped
drawing and the program did not end. Read off the stuck process with
`tools/probe/stacks.c`, which prints every thread's stack: the call waits
on a thread of the driver's own, one of those sits in
`D3DKMTWaitForSynchronizationObjectFromCpu`, and the window's thread, if
it resizes again, joins them inside the hook the driver has on the window.
It took the first rebuild after a pause, which is a swapchain with a
present still queued for the display. So the first of a run of rebuilds
lets the swapchain come to rest before it is replaced — the queue drained
and 20 ms for the refresh, in `recreate_swapchain()`, whenever the last
rebuild is more than a quarter of a second back — and with that none of
ninety starts stopped, and none of ninety with the rest before every
rebuild, against twelve of some two hundred and sixty without. Only the
first size of a resize pays it. The rule was first written on the
swapchain's age, and a second monitor found the hole in that: a window
moved to a monitor of another scale as it opened is a rebuild of a
swapchain a third of a second old, and one start in five of those
stopped on the HD 630. Dated from the last rebuild, none of twenty-five.
Looked for again with the rest taken out, on the HD 530 under a later
Windows build: none of eighty starts stopped whose first size came a third
of a second after the window opened, and the thirty-fifth did whose first
came after four seconds. That one's stacks are kept,
`docs/intel-swapchain-stall-stacks.txt`, beside the draft of a report to
Intel: the window's thread and one of the driver's wait on the same lock
inside `ig9icd64`, the window's by way of the driver's hook, and the
frame's thread waits in the create.
On the build with it, every round held: the bare one with captures,
Wayland 56, X11 74, wine 65, wine on Wayland 41, the two guests 69 each,
the first laptop 69 with its second monitor and the second 66, and the
two again on their Intel devices; validation with sync and best-practices
on the two native targets and on both laptops on each device, the layer
loaded; AddressSanitizer and ThreadSanitizer on the two native targets;
and a pacing sweep of all four local environments and of both devices of both
laptops, in which nothing stopped and no wait timed out. The laptops'
figures are the table's above to within a frame or two, and the local
ones 60 or 61 in every cell.

Four other arrangements left the rate where it was, which is why none is
in the tree: the window's thread kept out of a resize while a frame is in
the driver; the first swapchain made on the frame's thread; every posted
message pumped during the waits; and only the application's. The rounds
never met it: they resize by the border and by the size list. The warning
above is what it looked like in a log.

That probe found why a short run on Windows can print nothing, which the
guest's was blamed for: with stderr a pipe, the C runtime there buffers it
four kilobytes at a time, where C promises it is not fully buffered. The
echo flushed each line and the library's own — the device inventory,
`--frame-times`, every warning — did not, so they arrived in blocks or at
exit, and a program that was killed took the last block with it. The
default sink now flushes after each line (`emit()` in `src/core/error.c`):
on the laptop the inventory arrives as the window opens and a frame line
every half second after it, where the first of either used to come sixteen
seconds in. A caller's own sink was never affected, and a program's own
`stdout` still is, so the probe closes the window and does not kill it.
Thirteen trees rebuilt for it without a warning, and the rounds held on all
seven environments — Wayland 56, X11 74, wine 65, wine on Wayland 41, the two
guests 69 each and the laptop 66, with captures — as did AddressSanitizer
and ThreadSanitizer on the two native targets (53 and 74 each) and
validation with sync and best-practices on those two and the laptop.
`fenestal-testbed.exe --frame-times` on that laptop is `frame 16.66 ms`, 60
fps, `hold 0.00`: the high-resolution wait holds on Windows 11 itself, and
the 17–25 fps is gone there.

**A lifetime gives back what it took, on Windows itself.**
`fenestal-lifecycle`, twenty lifetimes on each laptop: handles and USER
objects flat on all four devices, GDI objects flat on both Intel ones (11
and 10 throughout) -- and one higher per lifetime on both NVIDIA ones, 12
to 31 on the GTX 1050 Ti. That one is the driver's. `tools/probe/gdi.c`,
with no fenestal in it, makes a window, an instance, a device and a
swapchain and takes them down again: on the NVIDIA device each such
lifetime keeps one GDI object, on the Intel device none, and fifty
swapchains recreated on one live device -- what a resize makes -- keep
none. So it is one a device, which is one an `fnl_init()` and one a
device-loss recovery, and never one a resize; the lifecycle round allows
exactly that on an NVIDIA device and names it when it does.

**A captured click stays the scene's, wherever the hidden cursor is.** The
round pushes a captured pointer to the top-left -- on Win32 the hidden
cursor is confined to the client area, so that is the bar and a resize
corner -- and presses there. Before `deco_hit()` answered content while
captured, `WM_NCHITTEST` handed that press to the window manager, and no
button event reached the caller: a build without the change fails the
check under wine, and with it both laptops pass it (69 and 65 checks, none
failed).

### Vulkan through DXGI

**Vulkan on Win32 makes a swapchain at every size, and Intel's driver is
slow at it.** The floor above -- 25 to 30 ms a size, 33 to 44 fps through
a drag at three quarters of a core -- is the driver's create and destroy.
`VK_EXT_swapchain_maintenance1`, which would let one swapchain present
into a window of another size, is NVIDIA's and not Intel's. DXGI resizes
in place, as Direct3D 12 does (*A swapchain larger than the window*), so
where a swapchain's rebuild is slow the frames go to the window through a
DXGI swapchain instead (`present_dxgi.c`): the renderer draws into images
of its own that are Direct3D 11 textures shared with it, and a Direct3D 11
device on the same adapter, by LUID, copies the window's part into the
back buffer and presents. Direct3D 11 and not 12, by what the drivers
import (a probe on schenker):

| | HD 630 | GTX 1050 Ti |
|---|---|---|
| a Direct3D 12 resource as an image | refused | yes |
| a Direct3D 11 texture as an image | yes | yes |
| a Direct3D 12 fence as a semaphore | no | yes |
| `VK_KHR_win32_keyed_mutex` | yes | yes |

The texture's keyed mutex orders the two devices: a frame's submit takes
it at key 0 and gives it back at key 1, the copy takes it at 1 and gives
it back at 0. `d3d11.dll` is loaded at run time, the link line being the
system's own libraries. The buffers are larger than the window, shown
from its corner, so a size inside them is a copy of that much.

**Drawn in UNORM, the overlay a shade off.** The textures are
`B8G8R8A8_UNORM` and imported as that format, and the window's format is
that (`surfaceless_format()`): `srgb_surface` is false, the palette passes
through unlinearised, text coverage is encoded, and blending is in encoded
values -- *Colour*'s fallback, where Direct3D 12 on the same buffers draws
through `_SRGB` views and blends in linear. It stays so because only the
UNORM import is measured: the probe above, and `vk_dxgi_shares()`, ask
whether a Direct3D 11 texture imports as `B8G8R8A8_UNORM`. Either way to
`_SRGB` wants the same probe on the four laptop GPUs first -- textures made
`B8G8R8A8_UNORM_SRGB` and imported as `B8G8R8A8_SRGB`, as Direct3D 12's
images through Direct3D 11 are made `_SRGB` and copied into the UNORM back
buffer unconverted (`present_d3d11.c`); or the UNORM import made
`MUTABLE_FORMAT` with an `_SRGB` view, which an imported image may refuse
-- and the share test then asks for the format drawn, or a device passes
it and fails at the first import.

**Where it is chosen.** After the device is made and before anything is
built for the window (`vk_dxgi_choose()`): wherever the device can share
images with Direct3D 11, which is all four laptop GPUs and not the
guests' lavapipe. Never under wine (`vk_dxgi_shares()`): winevulkan
offers every extension it takes, and wine's DXGI then has no adapter by
the device's LUID and no shared handle -- the program did not start --
or, with DXVK's `dxgi.dll` beside vkd3d-proton, aborts in
`D3D11CreateDevice` on `DXGID3D10CreateDevice`. It was chosen where a
timed rebuild took over 5 ms, which kept the NVIDIA GPUs on their own
swapchains (1.5 and 3.5 ms),
and those are the slower way to the screen: PresentMon (2026-10-30, 60
Hz, vsync on, the window at rest) put the 960M's own composed by DWM, 73
ms from latch to screen, and the 1050 Ti's 36, where through DXGI each is
flipped onto a plane at 20; and flat out the 960M's own made 774 fps
against 871. What the default gives up is a Vulkan present a tool can
hook -- a capture or an overlay sees a Direct3D 11 one -- for which
`FNL_VK_DXGI=0` decides it for a window, as `1` does the other way. A
device that can share is listed twice, the second entry "Vulkan via
D3D11" (`fnl_gpu_via_at()`), and an entry chosen from the GPU box decides
it for that device. `pace.py` on schenker, frames presented and CPU:

| sizes/s | HD 630, own | HD 630, DXGI | GTX 1050 Ti, own | GTX 1050 Ti, DXGI |
|---|---|---|---|---|
| 15 | 45, 37% | 46, 3.6% | 49, 17% | 47, 6.0% |
| 30 | 44, 67% | 60, 7.0% | 61, 41% | 60, 15% |
| 60 | 35, 79% | 60, 7.0% | 58, 39% | 60, 17% |
| 250 | 35, 77% | 60, 9.1% | 62, 44% | 60, 16% |
| draw | 60, 5.2% | 60, 3.6% | 60, 8.9% | 60, 10% |

The ranking ranks a GPU and leaves the way to the renderer: a device
listed both ways is one key (`app_gpu_entry_id()`), measured once, on
the way Vulkan takes by itself (`GpuEntry::way_default`), and that entry
goes first of the two, where the ranking moves the window. Ranked apart
(`vulkan-d3d11|...`), each by how fast it draws flat out, where the copy
costs -- 786 against 1157 fps on the HD 630, 1256 against 2532 on the GTX
1050 Ti -- the way through DXGI never came first, and a program that
ranked was moved onto the device's own swapchain: on Intel a drag at 33
to 44 fps, and everywhere a refresh or three later on screen, which a
frame's cost does not measure. A rank file's keys of the two ways apart
are dropped as it is read. The rounds pass on both of schenker's
devices, with captures, 96 checks each.

**Three buffers, and two frames queued while a window is sized.** A
window being sized has buffers larger than it, which DWM composes, and on
aspire's display, which a WDDM 2.1 driver drives, a composed frame keeps
its buffer and the frame-latency object a refresh longer than on
schenker's. Vulkan through DXGI had two buffers: one DWM held and one the
copy waited for, and the frame handed over waited on that copy -- two
refreshes a frame, 30 fps through a drag on both of aspire's GPUs, its
latency wait 30 ms where schenker's was 2. Three hold 60 (`DXGI_BUFFERS`),
and flat out draw 715 and 1378 fps on the HD 530 and the 960M against 459
and 871. Direct3D 12 has three already, and with one frame queued drew 47
on the 960M and 42 on the HD 530 through Direct3D 11; it queues two while
the window is sized (`app_buffer_at_rest()`) and one at rest, and holds
60. Not the latency wait skipped while sizing, which held 60 too: the
late latch learns from that wait, and taught by frames that skipped it
it held nothing after -- 32 ms from latch to screen at rest against 19.8.
Two queued and back to one leaves nothing behind (20.1 ms). `pace.py`
(2026-10-31), sizes 30 to 250 a second, and from latch to screen at rest:

| | Vulkan, sized | Direct3D 12, sized | latch to screen |
|---|---|---|---|
| aspire HD 530 | 60 fps (30 before) | 60 (42) | 20.3 / 20.3 ms |
| aspire GTX 960M | 60 (30) | 55-60 (47) | 20.2 / 19.9 ms |
| schenker HD 630 | 60 | 60 | 20.1 / 20.2 ms |
| schenker GTX 1050 Ti | 60 | 60 | 20.1 / 19.8 ms |

The corner dragged by the harness, `border`: aspire 60, 60, 58 and 58
where it was 30, 30, 50 and 44. Which of the two paces better is then a
question of CPU, not of frames: Direct3D 12 sizes at 3 to 14% of a core,
Vulkan through DXGI at 6 to 21%, the NVIDIA GPUs the most on each.

**Vsync off tears, where the factory offers it.** The swapchain is made
with `ALLOW_TEARING` and every present carries it, and the frame-latency
object is not waited on (`vk_dxgi_present()`), as on Direct3D 12. Waited on,
it is released once a refresh as the display takes a frame and held the
rate to the display's: aspire, 60 fps through DXGI on both GPUs where the
960M's own swapchain made 774. Not waited on and not tearing, a present
queues behind the last, flipped at a refresh as well, and the queue fills:
60 fps still, and 132 ms from latch to screen on the 960M, the GPU starting
each frame 100 ms after it was submitted. Tearing, the keyed mutex paces
the frames: 871 fps on the 960M and 459 on the HD 530, and with vsync on
nothing changes (20.2 ms from latch to screen on both). A stamp of a frame
that tore is not a time it was shown (`app_present_number_shown()`).

Flat out against Direct3D 12 (2026-10-31, `1d987ac`, `pace.py --no-vsync`,
the draw row, each GPU named):

| | Intel's GPU | NVIDIA's GPU |
|---|---|---|
| schenker, Direct3D 12 | 2,122 fps at 34.6% | 869 at 49.7% |
| schenker, Vulkan through DXGI | 865 at 47.1% | 897 at 84.3% |
| aspire, Direct3D 12 | 3,338 at 66.9% | 1,254 at 65.9% |
| aspire, Vulkan through DXGI | 1,121 at 71.6% | 1,343 at 117.9% |

Direct3D 12 on the GPU that drives the panel is the fastest by two and a
half to three times, its frames crossing nothing; on the NVIDIA GPU the two
are within 7%. On Windows itself Direct3D 12 costs less CPU a frame than
Vulkan on every GPU -- 0.16 to 0.20 ms against 0.54 to 0.64 on Intel, 0.53
to 0.57 against 0.88 to 0.94 on NVIDIA -- where under wine on this desktop
it cost more (5,854 fps at 200% against Vulkan's 7,168 at 111% on X11):
vkd3d-proton's and DXVK's threads, not the API's.

### macOS: Cocoa, and Metal beside Vulkan

`src/backend/cocoa/cocoa.m` and `src/renderer/metal/` are driven by the round,
on the Mac (`macos-metal-native` and `macos-vulkan-native`;
tools/drive/README.md *Driving a window*): 64 checks with captures under each
tree, none failed, three not applicable in the first runs -- the move to
another monitor, with one there then; the drawn pointer, which is the window
server's; and a display mode from the titlebar, since the window server sizes
and places the window. The Mac has driven two panels since 2026-11-06 (below),
so the move is a check the round can run. Among them is
what the design assumed of AppKit: `miniaturize:` and `zoom:` on a borderless
window, its fullscreen on one, the resize loop and the drag, and the layer's
top-left gravity, the frame filling the window after eight resizes back to back
with none of it black. The third renderer there, OpenGL 4.1 through CGL, is
`macos-gl-native` (*OpenGL on macOS*). What follows is the design and the
reasons for it; what it still assumes and has not seen is TODO.md *macOS*'s.
What the first runs showed, started over ssh with nothing else on the desktop: a
program started from an ssh session reaches the window server as the logged-in
user and opens its window; both trees found the HD 630 and presented, the
testbed at 48 to 52 frames a second under Metal and 55 to 58 under Vulkan on
MoltenVK, on a 60 Hz panel, before going to rest (the difference was the
timer's, below, and not the API's); and minimal's window reported its 960x600
area, scale 120 and "Built-in Display" with 1920x960 of room, the panel less the
menu bar and the Dock. MoltenVK says once for each strip pipeline that Metal
cannot turn primitive restart off, which reaches only an indexed strip, and the
overlay's are drawn without indices. Captured on the Mac (`screencapture` over
ssh, which the Screen Recording permission for
`/usr/libexec/sshd-keygen-wrapper` allows, granted by hand), both programs drew
what they draw elsewhere under Metal: the testbed's bar, controls, panels and
pattern, and minimal's textured cubes, depth-tested, at 57 frames a second. The
Mac is schenker, booted from a USB stick into macOS 26 (`power.py boot schenker
macos`), its HD 630 the device and its GTX 1050 Ti hidden, so a frame there is a
real GPU's and not a rasteriser's.

**The Mac drives two panels, from power-up.** Until 2026-11-06 the stick
drove one at a time: with the iiyama PL4071UH on its HDMI, the built-in
panel was black while macOS called it online, the main display and powered
(no display sleep), and unplugging the external brought it back. Both show
together now, and four things make that so, each needed. The firmware's
hidden *Primary IGFX Boot Display* is LFP, where its default lights the
external alone. An OpenCore driver of the lab's (`PipeOff.efi`, run before
macOS) switches off every pipe the firmware lit beside the built-in
panel's: Apple's Kaby Lake driver scans that panel out of a framebuffer
slot in stolen memory, and only slot 0, the firmware's boot pipe, shows a
picture there -- booted on the external, the panel trained its link, took
slot 1 and stayed black; with the panel dark the driver also raises its
supply, or macOS finds no EDID over AUX and drops it. `igfxagdc=0` is the
third: with AGDC on, an external plugged in later stops at *AGDC managing
Display* and never gets a pipe. `igfxonln=1 igfxonlnfbs=0x01` is the
fourth: on a display sleep the framebuffer driver asks the HDMI port for
its status over DisplayPort AUX, times out and frees it, where the list
holds framebuffer 1 online; WhateverGreen ignores the list without
`igfxonln`. tools/drive/README.md *Driving a window* says what each is and
how the stick is set up. Both have survived a display sleep with the
external attached, and an external plugged in after login shows beside the
panel. The external's 3840x2160 mode is 30 Hz over that port and has shown
artifacts when set at boot; 2560x1440 @60 is the mode kept, set with a
display configuration -- `CGDisplaySetDisplayMode()` reports success and
changes nothing. A maximized window moved from Built-in Display to
PL4071UH and back, maximized at each step and home again (2026-11-05,
read from the window's reports: the built-in was dark then, so there was
no picture on it to capture). With both lit, the round moved the window
between them and back under each renderer -- 95 checks under Metal,
Vulkan and GL, none failed (2026-11-06).

**The window is borderless and the library draws all of it**, as on Win32.
A titled window with its bar made transparent was the alternative, and it
keeps a strip AppKit owns across the top of the content: a press there is a
window drag before the view hears of it, which is where the library's own
buttons are. So the style is borderless, resizable and miniaturizable, the
window says it can be key and main (borderless ones decline both by
default), and the edges are the inner margin `deco_hit()` already answers
for where a backend has no ring. A move is AppKit's own --
`performWindowDragWithEvent:`, handed the press, so it snaps and tiles as a
titlebar's would -- and a resize is the library's: no AppKit call resizes a
window from an edge it does not draw, so
`cocoa_begin_resize()` follows the pointer from the press until the release,
held at the floor, one `setFrame:` a step. Maximize is AppKit's zoom and
fullscreen its own fullscreen, a space of its own with the menu bar and the
Dock gone.

**A key pressed inside AppKit's fullscreen animation is the window
server's.** The library says the new state at the will, and the did
comes 560 to 670 ms later; a key pressed between the two never reaches
the window -- its release, after the did, does -- so a caller or a hand
that answers the state at once has its key dropped by macOS. A round on
the Mac pressed its next F11 0.3 s after the state and lost one of eight,
one round in two, ending in fullscreen after an even number of presses
(2026-10-31, read off a log of every ask and notification with times:
the eighth press drew no ask at all). The round waits the animation out
on the Mac (`WINDOW_ANIMATION`). And a fullscreen asked for through the
library inside the animation, which AppKit ignores there, is kept and
asked of AppKit from the did, or from a transition that failed, where the
window is not already so (`BackendState::fs_owed`, `fs_settle()`).

**Through AppKit's fullscreen transition the window server drops every
frame.** Between `windowWillEnterFullScreen` and `windowDidEnterFullScreen`
-- 600 to 670 ms on schenker, 560 the way out -- Metal's presented handler
reported every drawable but one with a `presentedTime` of 0, never shown,
and what the window showed was the last frame shown before the will. AppKit
resizes the window once, to the size it ends at, 57 ms after the will going
in and 11 going out, and sends no size between. A screen recording of the
testbed (`screencapture -v` on the Mac, frame by frame) showed what that
left: going in, the old 1280x754 frame standing in the corner of a screen
otherwise the window's background grey for 350 ms after the animation had
visibly ended; coming out, the full-screen frame cropped to the window for
500 ms. Nothing can be presented then, so the layer's gravity is what is
chosen: through the transition the held frame is stretched to the window
(`kCAGravityResize`, `layer_hold()`), which reads as AppKit's own zoom, and
pinned again at the did, or at a transition that failed. The size AppKit
resizes to is published at once like any other, so the first frame the
window server takes at the did is already at it. Pinned is
`kCAGravityBottomLeft`: an unflipped layer's top is its bottom edge, and
`kCAGravityTopLeft` held a frame at the window's bottom-left. A drag of the
corner holds the bar and the frame's top left still under the pointer, at
56 to 60 frames a second.

**The press is held until AppKit or the view says it ended.** The move is
asked for once the pointer is past the slop, a turn or more after the
mouse-down, and AppKit takes that mouse-down as late as that: a probe window
handed the press from its first drag event, from the turn after, or the
drag event itself moved under posted input alike. What AppKit's drag and
the resize loop keep is the release, so the view owes the caller one, and
pays it at the next move with no button down -- never at a drag, whose
press is the one the move is handed. Paid at a drag, the titlebar moved
nothing: the press was gone by the time the slop was passed.

**AppKit is the main thread's.** `fnl_init()` is refused on any other with
`FNL_ERR_NO_DISPLAY`, which the seam's rule already implies for the pump.
The render thread touches no AppKit object: what it asks of the window is two
atomics the delegate keeps (`on_screen`, and `occluded` from the occlusion
state), and what it presents into is the view's `CAMetalLayer`, which any
thread may use. The layer is the backend's, made with the view and hosted
by it, so both graphics APIs present into the same object: Metal directly,
Vulkan through MoltenVK's `VK_EXT_metal_surface`.

**The wakeup is a pipe**, whose read end is a run-loop source in the common
modes. A write is the one thing a signal handler may do, which rules out
posting an event from `wakeup()` itself; and the common modes reach the
tracking loops of a drag, a resize and a menu, where the pump is not the one
waiting. In the pump's own wait the source posts an event that ends it and
`fnl_run()` drains; in any other loop it drains itself, as Win32's
`FNL_WM_WAKEUP` does inside its modal loops.

**The view can outlive its App by one turn of the loop.** A window ordered
out is an exit for its tracking area, and AppKit's dequeue monitor may
route it on a later turn -- after `fnl_shutdown()`, and in a process that
opens another lifetime, into that one's pump. So the view detaches before
the window goes: its area is removed and its `app` cleared, a view with no
App installs no area when AppKit asks it to update, and the pointer
handlers return on a view that has lost it. The alternative -- an area
kept until the view is deallocated -- faulted in `mouseExited:` through a
NULL App at 0x140, the field's offset, when the bench's repeats closed one
lifetime and opened the next (2026-11-05, schenker, the first three-run
Mac bench; EXC_BAD_ACCESS on the main thread).

**Keys are virtual key codes**, which name a key by its place on the board as
`FnlKey` does, so `MAC_KEYS` is the whole translation. A PC keyboard's keys
land where macOS puts them: Insert on Help, Num Lock on the keypad's Clear,
Print Screen, Scroll Lock and Pause on F13 to F15. An ISO board reports the
key left of 1 and the one beside left Shift swapped against an ANSI one, and
they are swapped back (HIToolbox's `KBGetLayoutType`, asked by name so that
Carbon stays off the link line). A modifier is a `flagsChanged`, its side in
the flags' device-dependent bits, and Caps Lock reports the lock and not the
key, so each change is one press. AppKit sends no key release while Command
is held; a local monitor hands those to the window. **Text is AppKit's**:
in text entry the key goes through `interpretKeyEvents`, whose
`NSTextInputClient` answers are the composition (marked text, which a dead
key starts) and the committed text, and outside it a key types nothing. The
client is a sink and keeps no text: every editing command it is sent is a key
the caller has already had, and the marked text is the whole document, so
`markedRange` is `{0, length}` while there is one. Focus lost, or text entry
switched off, discards a composition still up (`discardMarkedText`), as X11
and Wayland drop theirs. A held letter repeats and types each time, as
`fenestal.h` promises of text entry, which macOS's press-and-hold does not
do: it holds the repeats back to offer accents, and the round's held A typed
nothing for its first five. `ApplePressAndHoldEnabled` is registered off as
the program's default, so a user who set it either way still has their way,
and the accents are the dead keys'.

**The view is given the keys back** each time the window becomes key and
each time it enters or leaves fullscreen. AppKit's fullscreen leaves the
window itself the first responder; a key then reached nothing while clicks,
which find the view by where they land, went on arriving.

**The capture detaches the cursor from the mouse**
(`CGAssociateMouseAndMouseCursorPosition`) and hides it, and the events'
deltas are the movement; it is handed back while the window is not key, as
Win32's clip is, and the cursor is put back where the reticle is when it
ends. A click into the content takes the pointer, as everywhere: the first
click on a window that is not in front is accepted only over the bar and
over what the caller claimed (`fnl_claim()`), a control either way, so that
bringing the window forward does not capture.

**The scale is the backing store's**, 1 or 2, so a library point is an AppKit
point unless `--scale` overrides it -- and then, as on Win32, the window's
pixels stay what AppKit gave and the library's points are those pixels at the
override.

**The layer is tagged sRGB**, which is what the frame's values are, and the
window server matches them to the display's profile. Untagged, they were
shown as the panel's own colours, unconverted: the bar's sRGB 41,41,46 came
back from the screen as 45,45,49, and a wide-gamut panel would oversaturate
everything. MoltenVK tags the layer the same way from the swapchain's
colour space, so the two trees' pictures agree.

**Metal is recorded afresh for every frame**, as Direct3D 12 and GL are: a
command buffer is used once. Every pipeline is made for the window's pass --
the drawable's `BGRA8Unorm_sRGB`, the window's `Depth32Float`, one sample --
and the scene's images are the same format, so one set of pipelines draws
into both, MLAA's pass taking the scene's depth for that reason only. A
command buffer keeps every object it names alive until it has run, so a
target or an atlas replaced while a frame is in flight is simply let go,
where Direct3D 12 drains its queue; only the overlay's arenas, which the CPU
writes, wait for their frame slot. The layer's `nextDrawable` is the
throttle, as Vulkan's acquire is, and `displaySyncEnabled` is vsync. The
drawables are three, and a window that is covered is the occlusion state's
to report, through the bridge, since the layer still hands out drawables
there that the window server never shows.

**A thread of the library's waits under the time-constraint policy** on
macOS (`fnl_thread_start()` in `core/thread.h`), for the reason Windows'
waits are on a high-resolution timer. The kernel coalesces an ordinary
thread's timer by a share of its interval -- the interval shifted right by
its tier's scale, capped per tier (`sysctl kern.timer_coalesce_tier*`):
measured on schenker from a program started over ssh, 1, 5 and 16.7 ms
waits came back 0.6, 2.4 and 7.2 ms late on average, alike for a condition
variable, `nanosleep` and `mach_wait_until`, and the testbed's 16.7 ms
spacing hold 3.5 to 4 late. That is past `PACE_LATE_MAX`, so each hold was
read as a stall and the next frame spaced from the late return: 46 to 52
frames a second on the 60 Hz panel, the same under Metal and under Vulkan.
Asking for the user-interactive class, which would cap the leeway at 1 ms
in a foreground application, moved nothing for one started from a shell;
`NSActivityLatencyCritical` moved nothing either. Under the time-constraint
policy -- aperiodic, a 5 ms share of a 10 ms constraint, preemptible, which
the kernel takes back from a thread that overruns it -- the same waits came
back 0.15 to 0.19 ms late, and both renderers drew 60 frames a second from
the first.

### Comparing the three

The claim the backends make is that a caller sees one window, so the
check worth running is the same sequence on more than one of them and a
comparison of what each *reports* — not of what the window server did, which is
allowed to differ. Two shapes, each made on Wayland, X11 and Win32:

*Driven*, by the one round that runs in every environment (see
tools/drive/README.md *Driving a window*). The
comparison that wants the window server's own account is made on X11 and on
Win32 under wine, where a window's rectangle can be asked for — a wine window
is an X11 window on XWayland. Open, pick 800x600 from the size drop-down, `F11`
in and out, maximize and restore: the window rectangle and the area the library
publishes agree at every step, and both round trips come back to exactly
800x600 with no drift.

*Undriven*, on those three, comparing the first state line at a range of
`--scale=`. It is the check for the Win32 sizing above: at 1.25 all three
report 1600x900 of area, where a creation size that missed the override
reports 1280x711. At 3.0 the two physical backends differ from Wayland, and
correctly — there the window stays 1280x754 *points* and only the buffer is
three times the size, so it fits the screen, while a physical window three
times as wide does not and the window manager clamps it.

## Checking it

There is no test target and no `install()`. Correctness is checked by the
checker variants; by what every build runs -- `verifyfont`, `verifykeymap`,
the fuzz harnesses, the ABI and header checks, `check-layering.py` and
`check-tables.py`; and by `tools/drive/stress.py`, which drives a real window
with synthetic input in every environment that will run here. It needs a
desktop, which is part of why there is no test target. `tools/drive/pace.py`
is the performance half of the same idea.

```sh
python3 tools/drive/stress.py                    # every environment
python3 tools/drive/stress.py x11-vulkan         # one of them
python3 tools/drive/stress.py --variant tsan     # under a checker
python3 tools/drive/stress.py --capture          # opt in to pictures
```

The round, the pace instrument and everything else that drives a window are
described in `tools/drive/README.md`: how a window is driven, the ledger that
records what ran where, and what the harness has had to learn about each
desk. Below are the checks themselves.

```sh
# Vulkan validation in the library's instance alone, with the two checks
# that are off by default (VK_LOADER_LAYERS_ENABLE='*validation' puts it
# in every instance of the process, Zink's and vkd3d-proton's too)
FNL_VK_VALIDATION=1 VK_LAYER_VALIDATE_SYNC=1 \
VK_LAYER_VALIDATE_BEST_PRACTICES=1 \
  ./build/linux/release/fenestal-testbed

# ThreadSanitizer; see tsan.supp for what is suppressed and why
./build.sh linux tsan
TSAN_OPTIONS=suppressions=$PWD/tsan.supp \
  ./build/linux/tsan/fenestal-testbed

# Address + UndefinedBehaviour, which share a build
./build.sh linux asan
./build/linux/asan/fenestal-testbed

# The public API called wrongly on purpose -- a NULL app at every entry
# point, descriptor sizes no build produced, overlay calls outside on_overlay,
# fnl_run() off its thread, fnl_shutdown() from a callback -- and then twenty
# lifetimes in one process, with descriptors and threads (handles and GDI/USER
# objects on Windows) after the last compared against after the first. Its
# first run found the wakeup eventfd and epoll set left open by every
# lifetime that installed the signal handlers; teardown now waits the
# handlers out and closes them.
FNL_BACKEND=x11 ./build/linux/asan/fenestal-dev lifecycle 20

# SIGINT, SIGTERM and SIGHUP at random moments -- before the window is up,
# while frames run, and a second one into the teardown the first began --
# under a checker. A run passes on a clean exit within its bound and a log
# with no report. 30 runs each, both native platforms, asan and tsan: none
# failed.
python3 tools/drive/signals.py x11-vulkan --variant tsan --runs 30

# Each checker made to see the bug it exists for, in every Debug tree:
# FNL_SELFTEST=asan|ubsan|tsan|assert commits one at fnl_init(), and this
# fails on a checker that stays silent; gl is an invalid GL call, committed
# by the OpenGL renderer once its debug context exists, which the context's
# output must bring to the log ("GL: GL_INVALID_ENUM in glEnable(GL_NONE)"
# on radeonsi). 32 controls over four trees, each Linux one under X11 and
# under Wayland, under Vulkan and under OpenGL; the win64 ones are a UBSan
# trap and an assert under wine, run with wine's crash debugger off, under
# Vulkan.
python3 tools/drive/selftest.py

# Memcheck, for the uninitialised read ASan cannot see. Only on lavapipe:
# valgrind cannot follow a hardware driver. valgrind.supp holds what is the
# driver's own, each entry anchored on a frame inside it.
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json FNL_BACKEND=x11 \
  valgrind --suppressions=valgrind.supp --error-exitcode=99 \
  ./build/linux/gc/fenestal-testbed

# Two more static analysers, beside -fanalyzer below: clang's, on a throwaway
# Debug tree, and cppcheck. Both reported what is now fixed and nothing else.
scan-build-21 --use-cc=clang-21 cmake -S . -B /tmp/sb \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFNL_FONT_DIR=$PWD/build/linux/release/generated
scan-build-21 --use-cc=clang-21 --status-bugs cmake --build /tmp/sb -j
cppcheck --std=c11 --enable=warning,portability,performance -D__GNUC__ \
  --include=/usr/include/wayland-util.h -Isrc -Iinclude -Itools \
  -Ibuild/linux/release/generated \
  --suppress=missingIncludeSystem src/core/*.c src/backend/*.c \
  src/backend/wayland/*.c src/renderer/vulkan/*.c

# GCC's static analyser, and a link with every unreferenced section discarded.
# Both are compile- and link-time passes with no runtime behind them, which is
# why the cross target takes them.
./build.sh linux analyzer
./build.sh win64 analyzer
./build.sh linux gc
./build.sh win64 gc

# What gc discarded. Off by default: it runs to tens of thousands of lines over
# a sweep, of which about ten are this project's own functions.
FNL_GC_PRINT=1 ./build.sh linux gc 2>&1 | grep "'\.text\.fnl"
```

**A driver's frames are named from its debug info, which the bare machines
are given.** The distributions strip their Vulkan drivers, so a report
through one reads `libvulkan_nouveau.so+0x41547e` and nothing more, and an
entry could key on the whole library -- every Vulkan call of ours. On
schenker's GTX 1050 Ti (NVK, Mesa 26.2.4) the Xorg round under
ThreadSanitizer failed on one such report in about one run in three: a
`free` on the render thread retiring a swapchain's images against the
`calloc` of the same block on the reaper's `vkDestroySwapchainKHR`. With
the driver's debug info fetched by build id from Arch's debuginfod, both
frames are `util_vma_heap_free()` (`src/util/vma.c`): the driver handing
GPU addresses back to its heap under the device's lock, a `simple_mtx`,
which is a bare futex TSan does not see. `tsan.supp` keys on that function,
Mesa's own list of free ranges and no memory of ours. libbacktrace, which
names frames for GCC's sanitizers, finds the info in
`/usr/lib/debug/.build-id/`, where `bare_driver.py --setup` puts it for
every installed driver; `--status` names a driver still without it. Where
it is missing the frame stays unnamed and the report fails the round, as
before. With the entry, eleven Xorg rounds under ThreadSanitizer on NVK, none
failed; in one, `print_suppressions=1` showed the entry matching the
race.

The same holds for Mesa's OpenGL and for the Vulkan layers, and `--setup`
fetches theirs too: libgallium, the library every gallium driver is (Zink,
iris, llvmpipe), the EGL vendor library in front of it, and each explicit
layer's library. The first KMS rounds under ThreadSanitizer on schenker's
GTX 1050 Ti on nouveau (Mesa 26.2.5, 2026-10-26) failed on ten reports
each, every frame between TSan's and our GL or Vulkan entry point Mesa's,
LLVM's or the validation layer's: Zink's shader cache handing a program
over from its cache thread to the render thread's readback, the GLSL type
cache, NVK's descriptor table under its own lock, lavapipe's compiles
inside LLVM, and a lock-order inversion between two of the validation
layer's per-object locks -- the layer in Zink's instance, since the
loader's variable the rounds then set puts it in every instance of the
process. A checker round now asks for it with `FNL_VK_VALIDATION=1`
instead, which names it in the library's instance alone, so Zink's goes
without it. `tsan.supp`
keys seven on the driver's function (and LLVM's on its library, which
nothing of ours calls), and says what each was checked against; with
them, the next round met three. Those three have no entry: each
innermost frame is inlined, and GCC's symbolizer (libbacktrace) names no
inlined frame in those libraries with the debug info installed, where
binutils' `addr2line` names them all from the same files -- so a key on
the function would match nothing, and one on the library would hide our
own (TODO.md). The layer's info is large (708 MB on Arch, libgallium's
366 MB), and a stick's persistent space, 3.5 GB free, holds it.

With the layer out of Zink's instance (2026-10-31, `1d987ac`, Mesa
26.2.6 with its debug info, on schenker and on aspire) its lock-order
report is gone, and what TSan meets on nouveau is Zink over NVK: 123 and
124 reports in the Vulkan rounds (which walk onto OpenGL), 142 and 143
in the OpenGL ones, some twenty signatures, every frame between TSan's
and our GL entry point Mesa's. Zink's threaded-context driver thread
(`gdrv0`, made inside our `eglCreateContext`) against the render thread
on one NVK command buffer, handed between them through Mesa's futex
fences, which TSan does not see; and Zink's cache threads (`zcq0`,
`zcfq*`) on the shader-cache hand-over that `race:blob_write_bytes`
keys, here inside NVK and unmatched. The innermost frame of every NVK
access is a fortified `memcpy` or `memset` inlined into the function:
GCC's symbolizer names 133 of 505 of NVK's frames and none of those,
`addr2line -i` all of them from the same file. A handful in libgallium
are named (`zink_update_rendering_info`,
`zink_create_gfx_pipeline_combined`, `create_batch_state`, the rwlock in
`create_program`). The layer, in Zink's instance, had been serialising
Zink's calls; out of it, NVK's races under Zink show, and both rounds
otherwise pass whole (71 checks, the two failures the report's).

**Why GCC's symbolizer names a driver's frame in one run and not the
next.** libbacktrace reads the list of loaded libraries once, at the
first frame it names, and never again: a driver the loader or EGL opens
after that stays `<null>` for the rest of the process, whatever debug
info is installed, and a `tsan.supp` entry keyed on its functions never
matches -- the "one run of two" of lavapipe's `_mesa_hash_table_copy()`,
and every innermost NVK frame on nouveau. Seen on schenker (2026-10-31):
`__sanitizer_symbolize_pc()` names `memcpy < blob_write_bytes` where NVK
was loaded before the first frame was named, and `<null>` once anything
was named before the `dlopen`. `external_symbolizer_path` changes
nothing, GCC's runtime never asking an external symbolizer while
libbacktrace is there; and `race:` matches the two accesses' stacks
alone, never where a thread or a block was made. So a bare machine's
`tsan` round preloads the open drivers behind TSan's runtime
(`TSAN_PRELOADED` in `bare_driver.py`: `libvulkan_*`, `libgallium*`,
`libEGL_mesa`; not the validation layer, which exports the loader's own
entry points and preloaded takes the loader's place). Named so, the keys
already there match -- `blob_write_bytes` 270 times in one round,
`blob_read_string` 114 -- and two more are added, Zink's pipeline compile
against its compile thread and NVK's pipeline cache, which the
specification makes internally synchronised. What is left is the gallium
threaded context's driver thread against the render thread on NVK's
command buffers, handed over through a futex TSan does not see; keyed,
NVK's command-buffer functions would hide a command buffer of our own
Vulkan renderer recorded on two threads, so a `tsan` round runs Mesa's
OpenGL with `GALLIUM_THREAD=0`, which makes no such thread. With both,
`kms-vulkan-bare@schenker:nouveau` and `kms-gl-bare@schenker:nouveau`
under `tsan` passed twice each, 71 checks and a clean log, and aspire's
two the same once each. This desktop's
own `tsan` rounds go through neither, and meet none of it.

A round on a kernel driver does not move the GPU to NVIDIA's own under
ThreadSanitizer, as it does not start on it: the window landed there
draws on NVIDIA's Vulkan driver, and the process died in TSan's
allocator (a SEGV, then "nested bug"): NVIDIA's own driver is checked by
ASan and not by TSan, below.

**An OpenGL build under ThreadSanitizer takes minutes.** On schenker the
instance alone took 80 s on Intel (iris), 98 s on llvmpipe and 204 s on
Zink over NVK, measured with the validation layer in Zink's instance; at
release each is under one. The GPU sections therefore wait on past their
own timeout for as long as the program says a move has begun and not
failed (`await_move()` in `stress.py`, up to ten minutes), rather than skipping
Zink under TSan: the races it then met were all the drivers', and our GL
renderer's threads against Zink's are worth the round's time. The next
round waited out a 202 s build there and landed.

**NVIDIA's own driver is checked by ASan and not by TSan.** Its ICD is
closed, so there is no debug info to fetch and an entry can key only on
the library. Under AddressSanitizer, on schenker's GTX 1050 Ti
(580.178.04, 2026-10-19), it leaves 746 to 790 bytes at exit: the D-Bus
connection `vkCreateInstance` opens, and one or two of Present's special
events polled in `vkQueuePresentKHR`. The latter are allocated inside
libxcb, where malloc's fast unwinder stops, so the stack reaches the
driver only with `fast_unwind_on_malloc=0`. `bare_driver.py` keeps the
ICD loaded, as it keeps lavapipe, and asks for the slow unwinder where
it does; `lsan.supp` keys on `libGLX_nvidia` and `libnvidia-glcore`, and
never on libxcb, which our X11 backend allocates through. With it the Xorg
and weston rounds pass under `asan`. ThreadSanitizer cannot run on this
driver at all: `libnvidia-glcore` does not import `pthread_create` but
looks it up with `dlsym` on `libpthread.so.0`, which finds libc's and not
TSan's interceptor, so TSan has no state for the driver's threads, and
the first `calloc` on one is a SEGV inside TSan's allocator (gdb: the
thread's cache at `0x8`). `__GL_THREADED_OPTIMIZATIONS=0` and
`__GL_SHADER_DISK_CACHE=0` change nothing. The harness says so and skips
the round; `FNL_GPU=intel` on the same boot, the driver still loaded,
passes `tsan` on Xorg and weston.

`build.sh` sets each sanitizer on the compile *and* on both linker variables:
the project links an archive and a shared object, and instrumenting only the
executable would leave nearly all the code out. The host tools -- `mkfont`,
`verifyfont`, `abidump` -- carry `-fno-sanitize=all` because they run
*during* the build: an instrumented generator is slower for nothing and fails
the build on its own exit status. `verifykeymap` and the fuzz harnesses are
left instrumented (`SANITIZED` in their `fnl_host_tool()` lines).
`verifykeymap`'s second half feeds the parser damaged keymaps, each in an
allocation of exactly its own length, and a harness replays damaged input the
same way: a read a byte past the end is something only a checker sees; they
are quick and free what they take, so neither objection applies.

The `gc` link is worth explaining, because the grep is what makes it useful.
`-Wall` already catches an unused **static**, so the only dead code that can
hide is a non-static function declared in `app.h` and never called — and with
`-ffunction-sections` an uncalled one is a section nothing reaches. It prints
thousands of lines either way, almost all `.rodata.VK_*` enum-name strings the
Vulkan headers emit, which is why the filter is on `.text` from this project's
own objects. **Expect no output.**

**Silence from a checker is only worth something if you know it was switched
on.** Sync validation reports nothing either when the code is correct or when
the setting was ignored, so it is worth confirming it bites: delete the render
pass's external subpass dependency and the same run produces ten
`SYNC-HAZARD-WRITE-AFTER-READ`. The same sentence applies to the analyser tree:
it is an ordinary incremental build, so a tool built into it once is never
recompiled and never re-warned about. Build it from scratch when the claim
matters.

Both `analyzer` trees report nothing, the host tools included. Three
buffers are zeroed to get there, each where the analyser cannot carry an
invariant that holds: the scratch block an atlas is decoded into in
`src/renderer/vulkan/text.c` and `verifyfont`'s copy of it, because a decode
that succeeds has written every byte and `-fanalyzer` cannot follow that through
the range decoder, whose match copy reads back only what the same decode wrote;
and `mkfont`'s cell array, which a fill loop writes whole before the dedup pass
compares it. A `calloc` in place of a `malloc` costs nothing that shows beside
the decode, and is preferred to a pragma, which would hide the next warning too.
So any warning from those trees is a real finding.

The sanitizer trees say something only when a round is driven on them, and
the whole of it was last held from scratch — the three targets' trees
deleted, thirteen rebuilt in 162 s without a gcc warning — with wine on
Wayland in the tree, `monitor_work()` in `win32.c` and the `host` entry on
the backend seam. `stress.py --capture` on the two native targets is 56
checks on Wayland and 74 on X11 under `asan`, under `tsan` and under the
validation layer with sync and best-practices on, none failed and none of
the three with a line in any log. `minimal.py` took `--capture` too, for
one picture — the library's bar on the second consumer's window, as the
compositor shows it — and is 20 checks wherever a composited picture can be
had and 19 on X11 and wine on X11: at release on all six, the guests
included, and on the native two under each sanitizer and the validation
layer.

### Forcing the paths that never run

Fallbacks and recovery paths by definition never run on a machine that is
working, so nothing exercises them unless someone forces it. Most are named
fault sites (`src/core/fault.c`), switched on in a Debug tree -- `asan`,
`tsan` or `gc` -- by `FNL_FAULT` in the environment; a bare name fires every
time the site is reached, `@N` the Nth time only, `%N` every Nth and `*N` the
first N, and a list is comma-separated. Under `NDEBUG` every site is the
constant false. The rest are still one edit each.

```sh
FNL_FAULT='surface-lost@60,create-surface*3' FNL_BACKEND=x11 \
  ./build/linux/asan/fenestal-testbed
FNL_FAULT=scene-mlaa FNL_BACKEND=x11 \
  ./build/linux/asan/fenestal-dev lifecycle 6
```

`fenestal-dev lifecycle` is the program for the scene targets: its lifetimes
cycle through the antialiasing modes and pin the scene size every other
time, so the sixth reaches the scaled MLAA image.

| Path | Force it with | Expect |
|---|---|---|
| grayscale text (no `dualSrcBlend`) | `FNL_FAULT=no-dual-src` | legible text, no colour fringing |
| no multisampling | `FNL_FAULT=no-msaa` | FSAA inert, no multisampled pass built |
| no present stamps from the driver (Vulkan) | `FNL_FAULT=no-present-timing`, a Debug tree on KMS's display path or the Mac's MoltenVK | neither `VK_EXT_present_timing` nor `VK_GOOGLE_display_timing` enabled: the frames drawn as before and none stamped as shown, so no latch-to-screen figure there |
| UNORM surface | `FNL_FAULT=unorm-surface` | the palette *not* linearised |
| no `wp_viewporter` | `FNL_FAULT=no-viewporter` | integer `set_buffer_scale`; `--scale=1.25` rounds **up** to 2.00 |
| no unaccelerated relative motion (Wayland) | `FNL_FAULT=wl-accelerated-relative`, a Debug tree on Wayland, the pointer captured | `relative motion: the compositor gives no unaccelerated delta; accelerated instead`, once; the reticle moved by the desktop's curve. Run 2026-11-03: `wayland-vulkan` on the `gc` tree, the line once and the round's 75 checks otherwise clean |
| the first backend finds no window server | `FNL_FAULT=backend-no-display@1` on a desktop with `WAYLAND_DISPLAY` and `DISPLAY` both set, `FNL_BACKEND` unset | `Wayland refused (FNL_FAULT)`, then `backend: X11; passed over Wayland: Wayland refused (FNL_FAULT)` and an X11 window |
| the first renderer will not draw here (a build of several) | `FNL_FAULT=renderer-refused@1` in a Debug `win64` tree, `FNL_RENDERER` unset | `renderer: Vulkan; passed over Direct3D 12: Direct3D 12 refused (FNL_FAULT)`, on the same window. Run 2026-11-03: the `win64 gc` tree under wine, `fenestal-dev pace`, the line said and Vulkan drawing the window |
| a move to another kernel driver is refused | `FNL_FAULT=gpu-driver-refused` in a Debug tree with `FNL_GPU_DRIVER=switch`, then the GPU box onto `<GPU>, on <driver>` | drawn across first where it can be (`GPU driver: drawn on … while … moves to …`), then `GPU driver: … stays on … (FNL_FAULT)`, `Vulkan would not draw here (the GPU did not move); back to …`, and the window drawn again on the GPU it had; nothing run as root. Run on schenker, KMS, with `tools/probe/drivermove.c` in place of the box |
| a move to another kernel driver lands on another GPU | `FNL_FAULT=gpu-driver-missed`, the same | `Vulkan would not draw here (drawn on …, not on the GPU moved); back to …`, then `GPU driver: … back to …`, the GPU on the driver it left and the window drawn again where it was. Run on schenker, KMS, as above |
| a move to another kernel driver is not drawn across | `FNL_FAULT=gpu-driver-uncrossed`, the same | `GPU driver: not drawn across the move (FNL_FAULT)`, `GPU driver: the frames pause for this move and every later one`, and the move made as before, the window drawn on the GPU moved; the next move paused without asking. Run on schenker, KMS, gc tree, a move each way: 1.40 and 0.77 s without a frame |
| something of NVIDIA's driver is left in the process before a move | `FNL_FAULT=gpu-driver-leftover`, the same | `GPU driver: (FNL_FAULT) is open in this process with no renderer built, so … stays on …`, `GPU driver: no move offered from now on: …`, the window drawn again where it was, and the list without the move entry. Run on schenker, KMS, gc tree, the GPU on nouveau |
| a move to another renderer's GPU will not build | `FNL_FAULT=renderer-move-refused@1` in a Debug tree, then the GPU box onto another renderer's GPU | `… would not draw here (… refused (FNL_FAULT)); back to …`, and the window drawn again with the renderer and the GPU it had |
| KMS with no keymap data | `FNL_FAULT=kms-no-keymap` | one line, `no keymap for layout …`; keys arrive and text entry types nothing |
| KMS with no cursor plane | `FNL_FAULT=kms-no-cursor` | one line, `… so the pointer is not shown`; the round still aims by the echo |
| the glyph atlas outside VRAM | `FNL_FAULT=atlas-out-of-vram` | `the glyph atlas is in system memory: …` where the device allows one (NVK); `… is in VRAM, the only memory this device allows it …` where it does not (`anv` on the HD 630); the text unchanged either way |
| KMS with no device for the display | `FNL_FAULT=kms-no-display` | `KMS: FNL_FAULT: no device takes …; drawing offscreen and copying each frame onto …`, and the copy path on a device that has a display |
| KMS's copy path with no headless surface | `FNL_FAULT=kms-no-display,kms-no-headless` | `KMS: no headless surface for the GPU that will draw; it draws into images of its own`, and the frames by the copy path from the renderer's own images, as NVIDIA's own driver has them without the fault. Run under the fault on both laptops (gc, nouveau): Intel's, NVK and llvmpipe, each move |
| OpenGL on KMS by the readback where it could draw onto the output | `FNL_FAULT=kms-gl-copy`, a Debug `linux` tree on KMS, `FNL_RENDERER=gl` | `KMS: OpenGL copies each frame onto the output: FNL_FAULT`, once, and frames by the pack buffer's readback. Run 2026-11-03: `kms-gl-bare` on the thinkpad, `gc`, the line once and the round's 56 checks otherwise clean; the same path runs unforced on NVIDIA's EGL beside an Intel panel |
| KMS with a mode or an output that will not show | `FNL_FAULT=kms-mode-refused`, and a mode or an output picked from the titlebar's lists | `KMS: … would not show WxH; back to … at WxH`, the lists' marks and the boxes back on what was, frames resume; on both paths |
| KMS with a display mode whose swapchain the kernel refuses | `FNL_FAULT=kms-swapchain-refused`, and a mode or an output picked from the titlebar's lists | `the display refused a swapchain on its mode` twice, then `KMS: … would not show WxH; back to …`, frames resume; display path only |
| X11's sync value released at the flush, not when Present shows the frame | `FNL_FAULT=x11-sync-at-swap`, `x11-gl` (`FNL_BACKEND=x11 FNL_RENDERER=gl`) in a Debug tree, the border dragged | the drag as before the hold: `pace.py --border` 39 fps on this desk against 53 without the fault, frames with tens of ms of `other` |
| no refresh rate known on X11 | `FNL_FAULT=x11-refresh`, a Debug tree, `FNL_BACKEND=x11` | every monitor's rate 0, read as unknown: the frames paced by the measured period alone |
| no `FNL_STAMP` to take the clipboard's server time from (X11) | `FNL_FAULT=x11-clip-unstamped`, a Debug tree, `FNL_BACKEND=x11`, a copy and a paste | `the clipboard has no server time to stamp with; copy and paste at CurrentTime`, once; the selection taken and asked for at once, and every paste answered as before, none left to the deadline. Run 2026-11-03: `x11-vulkan`, `gc`, the line once and the round's clipboard checks and 107 others clean |
| a swapchain stuck out of date | `FNL_FAULT=stale-swapchain%300` | `every swapchain at the window's own size is out of date at once (0 in a row); the window's size is nudged` each time; on X11 the window a pixel wider and back, ending at its own size, frames continuing; elsewhere nothing more |
| a swapchain larger than the window refused (X11, Vulkan) | `FNL_FAULT=swapchain-scale-refused`, a Debug tree, `FNL_BACKEND=x11`, the border dragged | `a swapchain larger than the window was refused (FNL_FAULT); sizes recreate it`, once; sizes recreate the swapchain from then on |
| text past the baked ladder | `--scale=3` | glyphs the size the layout wants, resampled and soft |
| device-loss recovery | `FNL_FAULT=device-lost%300` | `device lost -- rebuilding`, then `rebuilt on …`, frames resume |
| surface-loss recovery | `FNL_FAULT=surface-lost%300` | as above, and the `VkSurfaceKHR` is replaced |
| a rebuild that fails *after* the surface came back | `FNL_FAULT=surface-lost@60,recover-objects@1` | `device lost -- rebuilding` again next turn, until it takes |
| a `create_surface()` that refuses | `FNL_FAULT=surface-lost@60,create-surface*3` | `surface lost -- rebuilding` *n* more times, then `rebuilt on …` |
| the handle that call leaves behind | as above, and write a junk `VkSurfaceKHR` into `VK(a)->vk_surface` before failing — which is what the spec permits a driver to leave there | SIGSEGV inside `libvulkan` from `recover_from_loss`, on the retry's `vkDestroySurfaceKHR`. Which is why every bridge (`surface_*.c`) builds into a local and assigns only on success |
| the retired-swapchain early flush | `a->resized = true` every frame, `submitted = false` after `draw_frame()` | `retired_count` reaches `MAX_RETIRED` and `free_all_retired()` runs |
| a half-built retired entry | `FNL_FAULT=framebuffers@2`, and a resize | four frames later `free_all_retired()` tears the entry down and the loop carries on |
| an offscreen target the device refuses | `FNL_FAULT=scene-target`, `scene-ms` or `scene-mlaa`, under `fenestal-dev lifecycle 6` | the first drops the whole target; the second costs FSAA alone and the third MLAA alone. One line through the log sink, once per target |
| the renderer says the window is not shown | `FNL_FAULT=occluded`, any Debug tree, under `fenestal-dev pace 0 6 --settle 1 --draw --no-vsync` | after the first frame nothing is drawn and the loop parks in `FRAME_WAIT_S` turns: 0.08 s of CPU in 7 s against 7.3 without, on this desktop's X11. Run |
| Direct3D 12 failed on a window with its swapchain on it | `FNL_FAULT=d3d12-after-swapchain@1`, a Debug `win64` tree, no renderer named and Direct3D 12 first | `renderer: Vulkan; passed over Direct3D 12: FNL_FAULT: after the swapchain`, and Vulkan's frames (through DXGI) on the same window. Seen on aspire's HD 530 and GTX 960M (2026-10-31, `FNL_GPU_RANK=off`); a later move to the 960M's Direct3D 12 lands on that window too, and back to Vulkan, no sample black |
| Metal failed on a window with its layer given the device | `FNL_FAULT=metal-after-layer@1`, the `macos gc` tree, no renderer named | `renderer: Vulkan; passed over Metal: FNL_FAULT: after the layer`, MoltenVK drawing the same NSWindow; with `framebuffers@1` beside it, `renderer: OpenGL; passed over Metal: ...; Vulkan: FNL_FAULT: create_framebuffers` and OpenGL drawing it (schenker, 2026-11-01) |
| a DXGI that will not present unscaled (Direct3D 12) | `FNL_FAULT=d3d12-no-scaling-none`, in a Debug `win64` tree | `the swapchain would not leave its frames unscaled (0x80070057); they are stretched while the window resizes`, once a device; the window draws as before. Run 2026-11-03: `win64-d3d12` under wine, `gc`, the line once and the round's 93 checks otherwise clean |
| a window DWM composes (Direct3D 12) | `FNL_FAULT=d3d12-composed`, a Debug `win64` tree on Windows, vsync on, the window at rest for 60 presents | `DWM composes this window's frames; they go through a Direct3D 11 swapchain instead`, then, measured there, `DWM composes the Direct3D 11 swapchain's frames too; back to this device's own`; frames continue on the device's own swapchain |
| the Direct3D 11 swapchain refused (Direct3D 12) | `FNL_FAULT=d3d12-composed,d3d11-present-refused`, as above | `the D3D11 present (forced)`, then `the Direct3D 11 swapchain could not be made; back to this device's own`, and the window on the device's own swapchain, measured no more. Not reached in a guest: the measure skips a software device, and the guests draw on WARP. Run 2026-11-03: schenker and aspire, both GPUs each, `gc`, the lines once a device and the rounds otherwise clean |
| the wheel by its message alone under wine (Win32) | `FNL_FAULT=win32-no-raw-wheel`, a Debug `win64-*` tree under wine | `raw mouse input unavailable; under wine a wheel step can be lost`, once; the wheel read from `WM_MOUSEWHEEL` as on Windows |
| input stamped by the tick alone on Windows (Win32) | `FNL_FAULT=win32-no-raw-thread`, a Debug `win64` tree on Windows, with `--frame-times` | `no raw input thread (FNL_FAULT); input latency is read in the tick's steps`, once; no `raw input:` tally at close, a key's `input` figure 0 or about 16 ms over one, and a capture registered on the window as before. Run 2026-11-03: `win64-vulkan-vm` on win10 and win11, `gc`, the line once and 91 checks otherwise clean each |
| no input method composition reported (Win32) | `FNL_FAULT=win32-no-imm32`, a Debug `win64` tree, a dead key typed under wine on X11 | `no input method composition (FNL_FAULT): the system shows it in a window of its own`, once; the accent shown in wine's own box at the window's bottom-left and no `FNL_EVENT_COMPOSE`, the letter after it typed accented as before. Run 2026-11-03: `win64-vulkan` under wine, `gc`, the line once and the round's typing, dead keys included, clean |
| an opaque EGL surface refused (OpenGL on Wayland) | `FNL_FAULT=gl-opaque-refused`, and fullscreen | `the opaque EGL surface was refused (FNL_FAULT)`, then `surface lost -- rebuilding` once and `rebuilt on …`; the window keeps its alpha surface in fullscreen and is never asked again. Run 2026-11-03: `wayland-gl`, `gc`, the line, one `surface lost -- rebuilding` and `rebuilt on`, the round's 74 checks otherwise clean |
| OpenGL 4.1 on a 4.5 device | `FNL_FAULT=gl-41`, a Debug tree, `FNL_RENDERER=gl` | `GPU in use: … (OpenGL …), drawn through the 4.1 profile`, and the frames as at 4.5 (*OpenGL 4.1* has the rounds run so) |
| any one of the out-of-memory unwinds | `LD_PRELOAD` a `malloc` that fails the *n*th allocation whose caller `dladdr` places inside `libfenestal`, and sweep *n* | a clean `FNL_ERR_OUT_OF_MEMORY` out of `fnl_init()`, or one of the documented tolerances — `vk_find_present_queue()`, at each of its four call sites, and the pipeline cache's read-back and write-out. Never a crash, and nothing left behind across repeated lifetimes |

Four parsers are worth fuzzing rather than reading, each load-bearing on
input this program does not author, and each has a harness in `tools/fuzz/`
behind libFuzzer's entry point: a compositor's keymap (`fuzz_keymap`), the
rules and console files KMS resolves a layout through (`fuzz_rules`), the
atlas streams (`fuzz_font`) and the strings drawn (`fuzz_text`). Every native
tree builds them against `replay.c` and runs them on every build: the
committed corpus, then a fixed-seed run of damaged copies -- 3,000 keymaps,
3,000 rules files, 10,000 streams and 20,000 strings, a few seconds together
under ASan. The clang trees also build each
against libFuzzer (`fuzz_keymap_lf` and the rest) for a campaign, which the
build never runs:

```sh
build/linux/clang/fuzz_keymap_lf -max_total_time=600 \
    tools/fuzz/corpus/keymap
```

The first, two minutes each from the committed corpus, ran 132,859 keymaps,
2,204,207 streams and 424,488 strings and found nothing. Below, the hand
campaigns that came before the harnesses, and what they established:

* **`keymap_parse_xkb()`**, the compositor's keymap as text, parsed on the
  thread that pumps input; every key it fills is then typed at every
  modifier, so a map full of nonsense is one `keymap_type()` survives too.

* **`font_decode()` in `tools/font_codec.h`**, the decoder every atlas and the
  cell table come through. 640,000 damaged inputs under ASan and UBSan, each of
  the six shipped atlas streams and the cell table's in turn, with the input
  and the output allocated to exactly the lengths passed so that one byte
  either side is a report: one to eight bit-flips, the input cut short, the
  input run on with arbitrary bytes, arbitrary bytes throughout, and an output
  length shorter or longer than the stream encodes. No report, no write past
  the output, and no call that failed to return — the loop is bounded by the
  output length, and the slowest call under the sanitizers took a quarter of a
  second. **Not one of the 426,807 damaged streams decoded.** The format
  carries no checksum, but the decoder accepts only a stream it has consumed
  exactly and whose 32-bit code register has come back to zero, and damage
  leaves that register somewhere else: by construction about one damaged
  stream in four thousand million would pass, which is a by-product worth
  having and not a reason to trust a blob more than the executable it is
  linked into. The one thing that does slip through is a wrong *length* on a
  good stream: asked for up to 4096 bytes more than was encoded, 6 calls in
  106,856 succeeded, the flush bytes decoding as a symbol or two. The lengths
  are the generated header's, baked beside the stream, and `verifyfont`
  checks them. Beside that, 213,328 buffers of coverage-like bytes round-trip
  exactly through encode and decode.
* **`utf8_next()` and the glyph lookup**, which draw strings that come from the
  compositor and the driver. 300,000 hostile strings through `text_width()` at
  every size, and a run with overlong forms, surrogates, out-of-range code
  points, truncated sequences and lone continuation bytes installed into
  `gpu_name` and the output list so they reach `text_draw()` for real. Malformed
  input draws U+FFFD, which is what it is for.

## Invariants

The short list, for anything touching this tree. Each is a rule; the section
named has the reason.

* **A consumer links `fenestal` plus the system's own libraries, and
  nothing else**: every runtime that is not the system's is opened at run
  time (*One build per operating system*), and the shared object's link
  says so (`--no-undefined`). A library something needs is run as a
  subprocess, not added
  to the link line — see *Building*, *X11: xcb, not Xlib* and *KMS: no window
  server*, whose layout is read from xkeyboard-config's files and whose DRM
  and evdev are ioctls against the kernel's own headers.
* **A push-constant or instance struct changes in three places at once**: the
  shader, the C struct and the `_Static_assert` that pins it. The instance
  structs are in `src/core/app.h`; the push-constant structs are in
  `src/renderer/vulkan/vk_internal.h`.
* **A field added to `FnlDrawState` is added to `fnl_draw_state_eq()`**, or the
  frame never re-records when it changes — see *Recording*.
* **`src/core/` names no window server, no DRM or evdev, and no graphics
  API**, not one file excused; `tools/check-layering.py` checks it — see
  *Source layout*. A backend's or a renderer's state lives behind
  `App::backend_state` or `App::renderer_state`, never in `App`.
* **What the backends do alike is written once, in `src/core/`**:
  the hit test, the press, the drag, the reticle, the size gate. A backend
  translates; it does not decide — see *Two dispatch tables*.
* **The pointer's shape has one precedence**, `deco_cursor()`'s: capture, then
  a resize edge, then the caller's shape, then the arrow — see *The pointer's
  shape*.
* **Every filled overlay rectangle is a background** — see *Recording*.
* **The Wayland epoll set stays level-triggered** — see *Threading*.
* **X11 has two connections, and a reader must not cross between them**; a
  write may — see *Frame pacing and the off-screen skip*.
* **Per-image instance buffers are device-scoped, not swapchain-scoped**, and
  `images_in_flight` is seeded with the previous frame slot's fence — see
  *Per-image instance buffers*.
* **A layout figure derived from a measurement wants re-measuring whenever
  anything joins the row it is measured against** — see *Give-way rules*.
* **Grep for the *writers* of a shared field, not its readers.** A gate written
  in one backend's vocabulary reads as a permanent zero in the others;
  `FnlBackendVtbl::invites` is that made into a seam entry — see *Two dispatch
  tables*.
