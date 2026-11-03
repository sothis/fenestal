# The demos

Small programs built on Fenestal, and the ones to copy. Each lives in its
own folder here and is self-contained: its program, its scenes, its shaders
and the `CMakeLists.txt` that builds them. The library is the only thing
the demos share, and each takes it as a consumer does, by
`add_subdirectory` and `fenestal::static` (or `fenestal::fenestal`).

They are dedicated to the public domain (CC0 1.0, `LICENSE` clause 3), so a
program of your own may start from one and keep no notice. They are not the
testbed: `fenestal-testbed` is the application the library is developed
with, and `examples/` holds the API reference's snippets.

| Demo | Renderers | What it shows |
|---|---|---|
| [`hello`](hello/) | any the build has | The smallest Fenestal program: a window, the library's titlebar and a black scene, with no callbacks at all. |
| [`motion`](motion/) | Vulkan | One lit cube, turning for as long as the window is open. The cube is made in the vertex shader, so the scene has no buffers, only a pipeline per render target. |
| [`motion2`](motion2/) | Direct3D 12, Vulkan | `motion`'s cube with a scene for each renderer. Both on Windows, Vulkan alone elsewhere. The Direct3D 12 shaders are the same GLSL, translated by spirv-cross and compiled to DXIL by dxc. |
| [`texture`](texture/) | OpenGL, Vulkan | The turning cube with a texture made at start, a framed checkerboard: uploaded through a staging buffer into an image with a descriptor set under Vulkan, and as a texture with a sampler under OpenGL, whose shaders are the same GLSL translated back by spirv-cross (4.50, and 4.10 for a Mac). Both on Linux and macOS, Vulkan alone on Windows. |
| [`texture2`](texture2/) | Direct3D 12, Vulkan | `texture`'s cube with a Direct3D 12 scene in place of OpenGL's: the texture copied from an upload-heap buffer on a list of its own and waited for with a fence, its view in a shader-visible descriptor heap, the sampler static in the root signature. Both on Windows, Vulkan alone elsewhere. |
| [`texture3`](texture3/) | Metal, Vulkan | `texture`'s cube with a Metal scene in place of OpenGL's: the texture copied from a shared buffer into a private one by a blit on the library's queue and waited for, the per-draw block handed to the encoder as bytes, the shaders the same GLSL translated to MSL by spirv-cross and compiled by the device at start. Both on macOS, Vulkan alone elsewhere. |

Every demo is built twice: `<name>` is linked against the static library,
and `<name>-dyn` against the shared one, which it loads from its own folder.

Keys, all of them the library's own:

| Input | Action |
|---|---|
| `F5` | show or hide the metrics panel: frame rate, frame cost, input latency, and the process's CPU, RAM, VRAM and GPU share |
| `F11` | fullscreen |
| Left click in the scene | capture the pointer; `F9` releases it |
| `Esc` | quit |

## Building

`./build.sh` builds every demo in every tree, through the demo's own
`CMakeLists.txt`, which the top-level build adds for each folder here that
has one. Each demo's programs land at the top of the tree:

```
build/linux/release/hello    hello-dyn    motion   motion-dyn
                    motion2  motion2-dyn  texture  texture-dyn
                    texture2 texture2-dyn texture3 texture3-dyn
                    libfenestal.so.0.9
```

A demo that has none of its renderers in a tree says so at configure and
is skipped.

### One demo on its own

Each demo's `CMakeLists.txt` also configures by itself, bringing in the
library from `../..`, as a project of your own would from wherever it keeps
Fenestal:

```sh
cmake -S demos/motion -B build/motion && cmake --build build/motion
```

That is a native build. For another platform, pass the toolchain file and
the tools that `build.sh` passes for that target, and on a Mac put MoltenVK
beside the programs as it does (`tools/moltenvk.py --into`).

## Running

```sh
./build/linux/release/motion
FNL_BACKEND=x11 ./build/linux/release/motion2-dyn
```

A Linux program opens Wayland, then X11, then KMS on a bare console, whichever
the session has; `FNL_BACKEND=wayland`, `x11` or `kms` picks one. Where a demo
has more than one renderer, or the machine more than one GPU (a software
rasteriser counts), Fenestal chooses which draws. The first run of each
program measures every GPU with every renderer the demo has, a second or more
each, while the window shows a calibration panel and takes no input; that run
and every later one draw on the fastest, kept in the user's cache
(`~/.cache/fenestal/` on Linux, `~/Library/Caches/fenestal/` on macOS,
`%LOCALAPPDATA%\fenestal` on Windows). So `texture` may draw with OpenGL on a
machine where OpenGL measured faster than Vulkan. `FNL_RENDERER=vulkan`,
`d3d12`, `gl` or `metal` names a renderer among those the demo has, `FNL_GPU`
a GPU, and `FNL_GPU_RANK=off` skips the measuring. Each program logs the
backend, renderer and GPU it chose. OpenGL wants a GPU with OpenGL 4.1 core,
which a Raspberry Pi 4's has not: `texture` draws with Vulkan there.

Keep a tree's files together when copying it elsewhere: the `-dyn`
programs load the shared library from their own folder, and on a Mac the
Vulkan demos load MoltenVK from there.

The Windows programs also run under wine. For `motion2.exe` or
`texture2.exe` on Direct3D 12 there, `tools/vkd3d_proton.py` stages
vkd3d-proton beside a copy of the program, since wine 10's own Direct3D 12
does not run DXIL:

```sh
python3 tools/vkd3d_proton.py --run build/win64/release/motion2.exe
```

## Adding a demo

Copy a demo folder here and rename it and the names in its
`CMakeLists.txt`: the project, the targets and the programs. The top-level
build finds every folder here with a `CMakeLists.txt`. A demo added to this
tree keeps the CC0 header its files start with.
