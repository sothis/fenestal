# Draft: Mesa's display WSI -- a stale plane, a leak

Not yet sent. One issue for Mesa, at gitlab.freedesktop.org/mesa/mesa, both
in `src/vulkan/wsi/wsi_common_display.c`, read at the tag `mesa-26.2.4` and
on `main` on 2026-10-18.

Read again on 2026-10-31 (`main` at 3d167c08, `mesa-26.2.6`,
`mesa-26.3.0-rc3`): the timeout, item 1, is fixed -- e102ea9f "vulkan/wsi:
fix a bunch of incorrect error code checks" (!45426, merged 2026-10-21),
which compares all four tests with `thrd_timedout`, and is in 26.2.6 and
26.3.0-rc2. It is left out of the issue and kept below as the record
`ACQUIRE_TIMEOUT_DISPLAY_NS` in `frame.c` rests on. Items 2 and 3 are
unchanged in all three, and the open merge requests on the file (!41188,
!44836, !40584) touch neither function; so the issue is those two.
Main's lines for the leak's frames are 2487 (`wsi_display_setup_crtc()`)
and 1557; the trace below is 26.2.5's.

Searched 2026-10-18 through GitLab's API, Mesa's issues and merge requests
("wsi_display_acquire_next_image", "display acquire timeout surface lost",
"thrd_timedout ETIMEDOUT wsi", "wsi_display_setup_crtc", "wsi display
formats leak"): nothing that matches.

## Summary

1. On a `VK_KHR_display` surface, `vkAcquireNextImageKHR` with a finite
   timeout that expires returns `VK_ERROR_SURFACE_LOST_KHR`, not
   `VK_TIMEOUT`, and marks the swapchain broken, so every later call on it
   fails the same way. The wait for a display fence has the same test, and
   answers a timeout with `VK_ERROR_DEVICE_LOST`.
2. A connector keeps the primary plane chosen for its first CRTC: given
   another CRTC later, `wsi_display_select_plane()` returns the cached
   plane, and the kernel refuses the commit ("Invalid CRTC for plane");
   every swapchain on it then fails with `VK_ERROR_INITIALIZATION_FAILED`.
3. Each time the last swapchain on a connector is destroyed and a surface
   on it is queried again, `connector->formats` is allocated afresh and the
   old array is never freed: 4 bytes a plane format -- 60 on an HD 530's
   primary plane, 156 on the Raspberry Pi 4's vc4. Still so in 26.2.6 and
   on `main` on 2026-10-31, beside !43528, which fixed the other leaks of
   that function.

## 1. The timeout (fixed in 26.2.6; not in the issue)

`wsi_display_acquire_next_image()` waits through
`wsi_display_wait_for_event()`, which returns `cond_timedwait_ns()`, which
returns `u_cnd_monotonic_timedwait()` -- a C11 result: `thrd_success`,
`thrd_timedout` or `thrd_error` (`src/util/cnd_monotonic.c` maps
`ETIMEDOUT` to `thrd_timedout`). The loop tests it against the errno:

```c
      if (ret == ETIMEDOUT) {
         result = VK_TIMEOUT;
         goto done;
      }

      ret = wsi_display_wait_for_event(wsi, timeout);

      if (ret && ret != ETIMEDOUT) {
         result = VK_ERROR_SURFACE_LOST_KHR;
         wsi_display_surface_error(chain, result);
         goto done;
      }
```

`thrd_timedout` is neither 0 nor `ETIMEDOUT`, so a timeout takes the second
branch. The display fence's wait (`wsi_display_fence_wait()`) is written the
same way, and its second branch answers `VK_ERROR_DEVICE_LOST`.
`wsi_display_wait_for_present()` in the same file tests `thrd_timedout`,
which is right.

Fix: compare with `thrd_timedout` in both places, and keep
`wsi_display_start_wait_thread()`'s `pthread_create()` error apart, since
that one is an errno.

### How it was met

A KMS program (no window server) presenting to an HDMI monitor through
`VK_EXT_acquire_drm_display` on anv, acquiring with a 100 ms timeout. The
monitor prefers 3840x2160 at 30 Hz, and the console had it at 1920x1080,
so the first presents wait out a mode set; the first atomic commit took
about 140 ms (strace). The third acquire timed out, came back as
`VK_ERROR_SURFACE_LOST_KHR`, and the program rebuilt its surface and
device -- every time it lit that output, at start and after moving back
to it from the panel. Under gdb, which slowed it, it did not happen.
Acquiring with 2 s, nothing is lost. No ioctl failed.

Clevo P64: Intel HD 630 (Kaby Lake GT2). Arch Linux, kernel
7.2.10-arch1-1, Mesa 26.2.4 (`vulkan-intel`).

## 2. The stale plane

`wsi_display_setup_crtc()` takes the connector's bound CRTC when it has
one and otherwise the first free CRTC its encoders allow, then asks
`wsi_display_select_plane()` for a primary plane of that CRTC. That
function returns `connector->plane_id` whenever it is set, without
checking the plane against the CRTC now chosen, and it is set from the
first call on. A connector that has been lit once, then left unbound by
someone else (a legacy `SETCRTC` with no framebuffer on its CRTC, from the
program holding master), and lit again on a different CRTC gets the new
CRTC with the old one's primary plane:

```
Invalid [CRTC:59:pipe A] for [PLANE:60:plane 1B]
```

and `vkCreateSwapchainKHR` fails, every time after.

Fix: in `wsi_display_select_plane()`, reuse the cached plane only when it
can still show the CRTC (`possible_crtcs` has the CRTC's index), and
choose again otherwise; or forget the plane with the CRTC where
`crtc_id` is reset.

### How it was met

The same program and Clevo P64, with its panel (eDP-1) and the HDMI
monitor. The window moved from the panel to HDMI and back through
`vkReleaseDisplayEXT` and `vkGetDrmDisplayEXT`, the one left switched off
with `SETCRTC`, and was then on the panel when its connector was forced
off (`echo off > /sys/class/drm/card1-eDP-1/status`) and the window moved
to HDMI: the swapchain for HDMI was refused as above, with HDMI given pipe
A and the plane cached from when it had pipe B. Switching outputs off by
the connector's DPMS property instead, which leaves the connector bound
to its CRTC, avoids it.

## 3. The formats leak

`wsi_display_setup_crtc()` returns at once while `connector->crtc_id` is
set, and otherwise selects a CRTC and a plane and copies the plane's
formats:

```c
   size_t size = sizeof(*plane->formats) * plane->count_formats;
   connector->formats =
      vk_zalloc(wsi->alloc, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
```

`wsi_display_swapchain_destroy()` sets `connector->crtc_id = 0` when the
connector's last swapchain goes, so the next `surface_format_supported()`
-- any `vkGetPhysicalDeviceSurfaceFormatsKHR` on a surface of that
connector -- runs it again over the array already there. Only
`wsi_display_free_connector()` frees one.

### Reproduction

A display plane surface on the device's first display, its formats queried,
then three times: a swapchain created on it, destroyed, and the formats
queried again. Nothing is presented and no mode is set; run with no
display server up, under ASan (`tools/probe/wsidisplay.c`, Vulkan alone).
Three arrays leak, one a round, the last freed with the instance. On an
HD 530 (anv, Mesa 26.2.5, Arch Linux), the driver kept loaded so it
symbolises:

```
Direct leak of 120 byte(s) in 2 object(s) allocated from:
    #0 malloc
    #1 vk_alloc        src/vulkan/util/vk_alloc.h:48
    #2 vk_zalloc       src/vulkan/util/vk_alloc.h:56
    #3 wsi_display_setup_crtc           wsi_common_display.c:2470
    #4 surface_format_supported         wsi_common_display.c:1555
    #5 wsi_display_surface_get_formats  wsi_common_display.c:1587
    ... vkGetPhysicalDeviceSurfaceFormatsKHR, after vkDestroySwapchainKHR
Direct leak of 60 byte(s) in 1 object(s) allocated from:
    ... the same, from the first query
SUMMARY: AddressSanitizer: 180 byte(s) leaked in 3 allocation(s).
```

And the same three on a Raspberry Pi 4 (V3DV, Mesa 26.2.5, Arch Linux
ARM): 156 bytes each, 468 in all, the frames offsets into
`libvulkan_broadcom.so`, which has no debug info there.

### How it was met

A program that makes its display surface again at each mode change, and
asks for its formats each time, leaks one array a change; LeakSanitizer
reports them at exit. Met on the Pi, whose output offers several modes;
the two laptops' panels offer one, which is why it went unseen there for
a while.

Fix: `vk_free(wsi->alloc, connector->formats)` before the allocation, or
keep the array while the plane is the same.

On 26.2.4 `wsi_display_setup_crtc()` also ends `return connector;` where
`ret` was meant, and leaks `drm_connector` and `mode_res`; `main` has fixed
those three.

## Beside the issue: a comment for #15248

Not part of the issue above: #15248 ("error with vulkaninfo", open,
label NVK, 2026-04-08) is this bug on a Blackwell laptop -- "Failed to
find required property 0 for object type 0xc0c0c0c0", then
`vkGetPhysicalDeviceDisplayPlanePropertiesKHR` failing with
`VK_ERROR_OUT_OF_HOST_MEMORY`. Its comments, read on 2026-10-31 (GitLab's
GraphQL API reads them with no login), already have the cause: marysaka
on 2026-04-15 (the connector has no `CRTC_ID`, a kernel bug suspected)
and on 2026-07-06 ("this works fine with `nouveau.atomic=1`", atomic by
default being worked on). So the comment credits that and adds only the
mechanism, a second generation and the error code; `wsi_get_connectors()`
on `main` (line 889) still answers a connector it cannot read with
`VK_ERROR_OUT_OF_HOST_MEMORY`.

> Confirming the `nouveau.atomic=1` finding above on a GTX 1050 Ti Mobile
> (GP107, NVK, Mesa 26.2.5, Linux 7.2.11). The path, for whoever takes
> it: without the parameter `drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC,
> 1)` fails with `EOPNOTSUPP`, the connector has no `CRTC_ID`,
> `find_properties()` fails it on that required property, and
> `wsi_get_connectors()` answers the whole query with
> `VK_ERROR_OUT_OF_HOST_MEMORY`, though nothing ran out. A GTX 960M
> (GM107) beside it has no connectors and never fails, which may be why
> this is not seen everywhere.
>
> Until atomic is the default: `wsi_display` cannot drive such a
> connector, so it could skip it (or report no displays for the device)
> rather than fail every display query with an out-of-memory error.

Measured on schenker's live key: `nouveau` without the parameter,
`drmSetClientCap` -1 and the connector with `DPMS` only; with it, rc 0 and
`CRTC_ID`. The key boots either way: its *nouveau* entry is the one
rounds have run on, *nouveau, atomic* the other (`fenestal-boot`).
