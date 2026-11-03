# Follow-up: work for the second GPU

An assessment of 2026-10-26 at `7d74df8`, read off the tree and nothing
built or run: where a machine has two GPUs -- schenker and aspire above
all -- the window draws on the one the ranking puts first and the other
idles. Can work of some kind go to the second to take pressure off the
first? Yes, a little, and the largest saving is not work moved but a copy
the machines' wiring already forces on every frame.

## What the machines are

| machine | CPU | outputs, all on | the other GPU |
|---|---|---|---|
| schenker | i7-7700HQ | HD 630: eDP-1, HDMI-A-1 | GTX 1050 Ti, no output |
| aspire | i7-6700HQ | HD 530: the panel | GTX 960M, no output |

Both are muxless: every frame the NVIDIA GPU draws has to reach Intel's
card.

- **X11 and Wayland**: PRIME, the driver's own copy.
- **Windows**: DWM's cross-adapter copy.
- **KMS**: ours, the copy path (DESIGN *KMS: no window server*). The
  frame is read back into a host buffer, the render thread waits the
  frame's fence, and the backend copies the pixels into a dumb buffer on
  Intel's card (`present_pixels`). On the panel that is some ten points of
  a core more than the display path. At 4K it is four times 1080p's, and
  it shows in the ranking: on schenker's HDMI-A-1 Intel's OpenGL ranked
  ahead of NVIDIA's Vulkan (TODO *The calibration, entry by entry*).

**One device, one queue.** The Vulkan renderer takes a single queue family
and queue (`src/renderer/vulkan/device.c`, `best_q`, `vkGetDeviceQueue`),
and the whole frame is drawn there. The second GPU is in the list as a
place to move to, nothing more.

**The library's own GPU work is negligible**: three overlay draws a frame,
a re-record of some 136 µs (DESIGN *Recording*), the font atlas baked at
build time. Moving the titlebar or the overlay to the other GPU for load's
sake buys nothing; what could pay is the hand-over to the display's GPU
and the caller's scene.

## Candidates, best first

### 1. The hand-over on KMS without the CPU

The NVIDIA GPU draws as now; its copy engine (a transfer queue, beside
the graphics queue rather than in it) copies the frame into a linear
dma-buf in system memory, and Intel's card scans that out, imported as a
framebuffer. That is what PRIME does under a window server, and it is the
"third way onto the output" DESIGN *KMS* already says is missing for
NVIDIA's own driver ("an image the display's device imports").

- Gone: the CPU copy, and the render thread's wait on the fence before
  `present_pixels`.
- The 4K ranking would measure the GPUs, not our copy.
- Needs `VK_EXT_external_memory_dma_buf` and
  `VK_EXT_image_drm_format_modifier` (linear). NVK has both; NVIDIA's 580
  branch is to be checked. Vulkan alone: OpenGL keeps the CPU copy.
- Fits the existing seam: the copy path is already a swapchain on a
  headless surface or images of the renderer's own
  (`RendererState::surfaceless`), so the change is what happens after the
  image is drawn.

### 2. The frame split where the scene target already splits it

The render scale's path (DESIGN *Render scale*) draws the scene into
the scene image and the window's pass draws one triangle over the area from
it, then the overlay. That is the natural cut: the scene on the NVIDIA
GPU, the window's pass -- the fetch, the overlay, the titlebar, the
reticle, the veil -- on the display's.

- **Less over the bus**: drawn at 1080p and stretched to 4K on Intel's
  GPU, about 8 MB a frame crosses instead of 33.
- **The titlebar and pointer at the display's rate**, even while the
  scene is slow or a move is under way; the veil could show the last
  scene image under a live overlay.
- **Costs**: a frame of latency unless pipelined with care; two devices
  that can each be lost; semaphores across devices (sync_file, or
  external semaphores); a pairing per renderer (Vulkan to Vulkan, Vulkan
  to OpenGL by `EXT_memory_object`, Direct3D 12's cross-adapter heaps and
  shared fences).

### 3. A helper GPU lent to the caller

An API addition: a second `FnlVulkanGpu` (and its Direct3D 12 twin), with
a way to share an image or buffer and a semaphore with the drawing
device. The scene stays the caller's; the library lends the device.
What a caller could put there:

- simulation: particles, physics, culling and LOD, procedural generation;
- textures: transcoding to BC formats, mip generation;
- post-processing, denoising;
- **video decode and encode on Intel's media engine**, which is fixed
  function and touches neither GPU's 3D engine -- the cleanest offload of
  all.

`on_gpu_created` and `on_gpu_lost` would have to bracket the helper as
well, and a move and the ranking would have to say what becomes of it.
It is a question of what the seam lacks, proved by a consumer that needs
it.

### 4. Calibration on the idle GPU (left out)

It would make the 31-33 s calibration run without holding the window.
Left out: the caller's scene would have to be built on two devices at
once, which the callbacks' one-device-generation contract forbids, and a
headless measurement leaves out the present path, which is what reordered
the 4K list.

## What does not pay

- **Alternate frames across the two GPUs** (SLI's way, Vulkan's device
  groups, Direct3D 12's linked nodes): those are for identical, linked
  GPUs, and with some five times between the HD 630 and the GTX 1050 Ti
  the frames would stutter.
- **Intel's GPU is not free.** It shares the package's 45 W, the cooling
  and the memory bandwidth with the CPU's cores; loading it can lower the
  clocks of the render thread and of the CPU copy, so a CPU-bound scene
  could get slower. Whatever is built is measured, per machine, as the
  ranking is.
- **Battery**: a helper keeps the NVIDIA GPU from runtime-suspending.
- **Metal**: schenker-mac shows the HD 630 alone (Pascal has no driver on
  Tahoe); nothing to share there.
- **Window servers**: the hand-over there is already the driver's, on its
  copy engines; only candidate 2 changes anything.

## Recommendation

1. **Measure first.** A heavy scene's `fnl_frame_gpu_ms()` on each GPU,
   to know whether there is GPU pressure to relieve at all; the testbed's
   scene has none. Measured at its heaviest (2026-10-31, aspire,
   `bench.py kms-vulkan-bare@aspire:nvidia` fullscreen at 1080p, MSAA,
   vsync off, the scene moving): the HD 530 6.0 ms of GPU a frame at 60
   fps (the display path holds FIFO), 36% of the period; the GTX 960M 5.1
   ms at 152 fps by the copy path. No pressure with this scene; a
   caller's heavier one is what would show any.
2. **Then candidate 1**: it fits the copy path as it is, closes an item
   DESIGN already names, and makes the 4K ranking honest.
3. **Candidate 2** after it, where the render scale is in use.
4. **Candidate 3** is an API decision, with the portable-scene work.
