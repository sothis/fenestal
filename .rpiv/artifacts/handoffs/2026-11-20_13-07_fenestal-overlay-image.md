# Handoff — the overlay image API

**Date:** 2026-11-20 · **Repo:** `fenestal` (`/home/json/ws/fenestal`) · **Branch:** `main` (clean, pushed, `722cd4e..37873c8`)

## Why this file exists

The previous session was implementing the overlay-image API you asked for
("the library should reupload a CPU copy. implement") when it began printing
tool calls as literal `<parameter>` XML — nothing ran, and the handoff it owed
never got written. This is that handoff, written after the work was finished.

## What is done (committed and pushed)

| Commit | What |
|---|---|
| `11af832` | `fnl_image_create()` / `_destroy()` / `_supported()` / `fnl_overlay_image()` — core, vtable, and pipelines in the **Vulkan** and **OpenGL** renderers |
| `2d450ac` | `.clangd` for `src/core` and `src/renderer`, so the internal headers parse alone in the editor |
| `37873c8` | `TODO.md` and `CHANGELOG.md` brought up to date |

The API's meaning is the decision you made: **the library keeps the CPU copy**.
`fnl_image_create()` reads the caller's buffer once, so it may free its own on
return; after a device loss / move / renderer switch the copy is uploaded
again. The calls are the render thread's alone (`[overlay]`: `on_overlay`,
`on_gpu_created`, or a `fnl_post()`), which is what lets them take no lock.

## How it was verified

- `examples/image.c` (the worked case and the check) draws under **Vulkan** and
  **OpenGL**, the right way up, scaled into its rectangle; Vulkan runs with
  `FNL_VK_VALIDATION=1` and the log is clean.
- A forced device loss (`FNL_FAULT=device-lost@5`, gc tree) rebuilds the device
  and the picture is still there on both renderers — the CPU-copy re-upload.
- ASan/UBSan round over the whole lifecycle (create → draw → destroy →
  create-left-alive → shutdown), device loss included: clean on both renderers.
- Gates: `tools/mkapidoc.py --quiet`, `check-layering.py`, `check-tables.py`
  all pass; the tree is warning-clean under clang.

One real bug was found and fixed on the way: leaving each image's Vulkan
objects to `vkDestroyDevice()` leaks the driver's allocations for them, so the
device-gone hook now destroys them explicitly (`vk_images_device_gone()`).

## What is owed

- **D3D12** (`src/renderer/d3d12/overlay.c`) and **Metal**
  (`src/renderer/metal/overlay.m`) pipelines: a PSO + SRV, and an encoder state
  + texture. Their vtables leave the three image entries `NULL`, so
  `fnl_image_supported()` answers false and `fnl_image_create()` refuses —
  the documented behaviour, not a silent blank. The shared shaders
  (`shaders/image.vert` / `.frag`) are already translated to HLSL and MSL at
  build time; nothing consumes them yet.
- **Ordering, unchanged from before:** text is one draw at the end of the
  frame, so a glyph appended before an image still lands on top of it, as it
  already did for a rectangle.

## Notes

- The `.clangd` files are editor configs for headers that are deliberately not
  self-contained (`vk_state.h`, `gl_state.h`, `backend.h`, `renderer.h` — all
  included *after* `core/app.h`). The compiler is fine; only a parser starting
  at the header is not.
- The library's default titlebar carries the FSAA / render-scale / GPU / size
  controls (`FnlInitDesc::no_titlebar_controls` is how a program drops them) —
  not the testbed's, as it looks.
