# Draft: nouveau's runtime suspend garbles tiled images in VRAM (Pascal)

Not yet sent. Two places:

1. **An issue for the kernel driver**, at gitlab.freedesktop.org/drm/nouveau:
   the cause is there (section 1).
2. **A comment on Mesa's #16535**, "[NVK][Kepler/GK106] Severe font glyph /
   alpha corruption in Vulkan applications" (opened 2026-10-10, no comments
   yet): its reporter runs a hybrid laptop with `DRI_PRIME=1`, whose
   discrete GPU runtime-suspends the same way, so it is very likely this
   (section 2).

Searched 2026-10-15 through GitLab's API, Mesa's issues and merge requests
("nvk pascal", "nvk GP107", "nvk text corruption", "nvk garbled", "nvk
tiling corruption" and others): #16535 is the only match. drm/nouveau's
tracker, searched the same day ("runtime pm corruption", "resume
corruption", "suspend vram", "runtime pm nvk", "d3cold" and others), has
runtime PM reports of crashes, hangs and failed resumes and none of image
contents.

`tools/probe/nvkatlas.c` is the reproducer: plain Vulkan, one file, no
window.

Read again on 2026-10-31: the same bug was reported on Kepler the day
before, Mesa #16729, "NVK: Textures corrupted after runtime
suspend/resume on Quadro K3100M (GK104)" (opened 2026-10-30, label NVK,
no comments): Mesa 26.2.5 and main, kernel 7.2.12, `power/control=auto`
at 5000 ms; `vkcube --use_staging` reproduces it, linear textures and
`power/control=on` do not, nouveau's OpenGL does not; and fprobe traces
show the suspend moving the buffers from VRAM to system memory with their
page kind (0xfe) kept, and nothing moved back after resume. So the first
place is now **a comment on #16729** (section 3), which adds Pascal,
this reproducer and its controls -- the image placed in system memory
from the start keeps its texels, where one evicted there with its VRAM
kind does not, which is the contrast to their trace. The drm/nouveau
issue (section 1) is still not there (drm/nouveau#511, TU117 text
artifacts at idle on two external monitors, is scanout, not this); filed,
it links #16729. #16535 has one comment now (2026-10-23, a GT 710 on
Debian 13, Mesa 26.1.6: "happens with all vulkan apps") -- a desktop card,
which nouveau does not runtime-suspend by default, so the comment there
stays a question and points to #16729.

## 1. Issue for drm/nouveau

### Summary

On a hybrid laptop whose NVIDIA GPU runtime-suspends when idle, a Vulkan
image with optimal tiling that NVK placed in VRAM comes back from a
suspend and resume with its texels in the wrong places: about half of
them wrong, in blocks, as if the data were restored without its tiling.
The image is never touched by the application in between.

### Hardware and software

Clevo P64: Intel HD 630 (Kaby Lake GT2, drives the panel) and NVIDIA GTX
1050 Ti Mobile (GP107, `10de:1c8c`). Arch Linux, kernel 7.2.9-arch1-1,
Mesa 26.2.4 (`vulkan-nouveau`). nouveau logs `pmu: firmware unavailable`.
`/sys/bus/pci/devices/0000:01:00.0/power`: `control` auto,
`autosuspend_delay_ms` 5000; every resume logs `Enabling HDA controller`.

### Reproduction

`nvkatlas` uploads a 1024x512 `R8_UNORM` image (`VK_IMAGE_TILING_OPTIMAL`,
device-local) once with `vkCmdCopyBufferToImage`, samples it at every
texel's centre through a `NEAREST` sampler into a target of its own size,
reads that back and compares it with what was uploaded; then sleeps, and
does the same again. One device, one queue, no window.

| | texels wrong after the sleep |
|---|---|
| NVK, 12 s, runtime PM `auto` | 261,090 of 524,288 |
| NVK, 3 s (under the 5 s delay), `auto` | 0 |
| NVK, 12 s, runtime PM `on` | 0 |
| Intel HD 630 (anv), 12 s | 0 |
| NVK, 12 s and 20 s, `auto`, image in system memory | 0 |

The image's `memoryTypeBits` is 0x7: VRAM (type 0, `DEVICE_LOCAL`), the
mappable window onto it (type 1, `DEVICE_LOCAL|HOST_VISIBLE|
HOST_COHERENT`) and system memory (type 2, `HOST_VISIBLE|HOST_COHERENT|
HOST_CACHED`). Placed in type 2 (`NVKATLAS_HOST=1`), the image comes
through every suspend whole.

The same reproduces every time. A longer-running program shows it as text
garbled for good after a pause (that is how it was found: a window's
glyph atlas, every glyph in place and in colour, its texels shifted in
blocks); allocating the atlas again does not help, since the new one is in
VRAM too and the next suspend does the same. With `NVK_DEBUG=gart`, which
keeps NVK's memory out of VRAM, the text stays whole.

### What is asked

Whatever restores VRAM on runtime resume to keep the memory's layout (page
kind) for buffers of tiled images -- or, if that is not possible on this
generation, not to runtime-suspend while such buffers are live.

## 2. Comment for Mesa #16535

This may well be the same thing, from the kernel side. On a hybrid laptop
with a Pascal GTX 1050 Ti (NVK, Mesa 26.2.4, kernel 7.2.9) glyph atlases
came out garbled the same way, and the cause turned out to be nouveau's
runtime power management: when the discrete GPU powers off while idle and
on again, an optimal-tiling image left in VRAM comes back with its texels
in the wrong places. A plain Vulkan program that only uploads an image,
waits 12 seconds and samples it again shows half the texels wrong; with
`echo on > /sys/bus/pci/devices/<gpu>/power/control` it is always correct,
as it is with a wait shorter than the 5 s autosuspend delay, and with
`NVK_DEBUG=gart`. With `DRI_PRIME=1` on a K2100M laptop your discrete GPU
runtime-suspends too; does the corruption go away with `power/control` set
to `on`? (Report filed against drm/nouveau: link.)

## 3. Comment for Mesa #16729

The same here on Pascal, so it spans generations: a GTX 1050 Ti Mobile
(GP107) on a hybrid Clevo P64, NVK with Mesa 26.2.4, kernel 7.2.9,
`power/control` auto at 5000 ms. A one-file plain
Vulkan reproducer, no window, that counts wrong texels: it uploads a
1024x512 `R8_UNORM` optimal-tiling image once, samples every texel,
sleeps, and samples again (attached, `nvkatlas.c` and its shader).

| | texels wrong after the sleep |
|---|---|
| 12 s, runtime PM `auto` | 261,090 of 524,288 |
| 3 s (under the delay), `auto` | 0 |
| 12 s, runtime PM `on` | 0 |
| 12 s and 20 s, `auto`, image in system memory (memory type 2) | 0 |

In the program it was found in (a window's glyph atlas), `NVK_DEBUG=gart`
kept the text whole as well.

The fourth row fits your trace: an image NVK placed in system memory
from the start comes through every suspend whole, and one placed in
VRAM and moved there by the suspend, its page kind kept, does not. A new
image does not help either, since it lands in VRAM and the next suspend
does the same.

## Owed before these are sent

- **Posting**, from the account that files them: the comment on #16729
  first; then the nouveau issue, linking #16729; then the comment on
  #16535, pointing to both.
- The figures are Mesa 26.2.4's; 26.2.6 is current (2026-10-28), so a
  run of `nvkatlas` on it is worth having first.
