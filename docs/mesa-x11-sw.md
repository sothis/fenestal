# Draft: two reports to Mesa

Drafts for Mesa's issue tracker (component: Vulkan WSI, X11), not yet sent.
Both are in `src/vulkan/wsi/wsi_common_x11.c`, both reach only the software
path that lavapipe takes, and both are in the source of 25.0.7 and 26.2.4.
`tools/probe/wsisw.c` reproduces each with nothing but Vulkan and xcb: `pad`
on a ThinkPad Edge (Core i3 M 370, Ironlake graphics, no Vulkan driver of
its own) under Xorg 21.1.16 and Mesa 25.0.7, and `shm` on Xwayland under
GNOME and Mesa 26.0.8, since the first machine's server offers no DRI3 and
Mesa then never uses MIT-SHM. The end says what the tracker held, and
that both are still in main.

## 1. The software swapchain never detaches its MIT-SHM segments

### Summary

With MIT-SHM, each swapchain image is a segment attached in the X server
(`xcb_shm_attach`) with a pixmap on it. `x11_image_finish()` frees the
pixmap and the client's own mapping, but never sends `xcb_shm_detach`, so
the server keeps every segment attached until the client disconnects. A
program that rebuilds its swapchain often -- one being resized -- runs the
system out of segments.

### Reproduction

`wsisw shm`: a 64x64 window, a three-image swapchain, rebuilt 200 times
with `oldSwapchain` and the old one destroyed. The process's segments still
in `/proc/sysvipc/shm`: 3 at the start, 78 after 25 rebuilds, 153 after
50 -- three more a rebuild -- and 603 after 200, every one of them still
there after the last swapchain is destroyed. They go when the connection
closes.

### What happens to a program

At 250 swapchain rebuilds a second, the client's `shmat` and `shmdt` stay
within nine of each other every second, and its creates and destroys within
three. Meanwhile, the segments marked for removal but still attached climb
by 380 a second, until they reach the system's limit (`kernel.shmmni`,
4096) after about 1,360 rebuilds. After that every `ShmAttach` fails with
BadAccess and every present with BadDrawable. A swapchain destroy then
waits for a Present event that never comes, and the program hangs.

The GLX path had the same leak and was fixed in 2019; this is the Vulkan
WSI's copy of it.

### What is asked

`xcb_shm_detach` for the image's segment in `x11_image_finish()`, before
the segment is released.

### Workaround

`MESA_VK_WSI_DEBUG=noshm`: nothing is attached and the leak goes. That
mode is where the second report comes from.

## 2. Without MIT-SHM, a present sends the row padding with the image

### Summary

`x11_present_to_x11_sw()` sends the whole image in one `PutImage` that is
`row_pitches[0] / 4` pixels wide: the padded row, not the image. Lavapipe
pads a linear row to 64 bytes, so an image whose width is not a multiple
of 16 pixels sends up to 15 columns of uninitialised memory to the right
of its content. Normally the window clips them. But an image of the old
width that lands after the window has been made wider shows them as noise
down the newly uncovered strip.

### Reproduction

`wsisw pad`, with `MESA_VK_WSI_DEBUG=noshm` (or on any server without
DRI3): a window 1001 pixels wide on a green background, an image of that
width cleared red and acquired, the window widened to 1033, and the image
presented. Read back at mid-height: x 999 and 1000 red, x 1001 to 1007
black -- the padding to 1008 pixels, zero this time -- and green from 1008
on. Seven of the 32 new columns written over, and exactly the padding.

### What happens to a program

A window with no manager, widened by its edge in 24-pixel steps and
captured straight after each step. The noise was exactly as wide as the
padding: 2 columns after a width of 1086 and 10 after 1254. A window
widened the same way that nobody presents into shows pure black there,
which is the server's own background. Widths in whole 16-pixel steps show
no noise at all.

The branch that sends one rectangle at a time sends `rect.width` and is
right, but this path never reaches it: `x11_image_init()` returns before
creating the image's `update_region` when `sw && !has_mit_shm`, and
`x11_queue_present()` only fills the rectangles when that region exists.

### What is asked

Either send `chain->extent.width` pixels a row -- a `PutImage` takes no
stride, so that is a row at a time, or a tightly packed copy -- or zero
the padding when the image is created.

### Workaround

None a client can apply to the padding itself: it is outside the image's
extent and belongs to the swapchain's memory. Fenestal makes the width of
a drag it runs itself whole 16-pixel steps, which depends on lavapipe's
alignment.

## Searched, and still in main

- **The tracker, searched 2026-10-15** through GitLab's API, which the
  browser check in front of the web search does not guard: issues and
  merge requests, open and closed, for `xcb_shm_detach`, `shm detach`,
  `shm segment leak`, `x11_image_finish`, `x11_present_to_x11_sw`,
  `noshm`, `PutImage padding` and `PutImage stride` among others. Neither
  is reported. The nearest are #121 and !340, the GLX leak fixed in 2019;
  #5259, speed and not either fault; #16335, a dri-sw leak per EGL dma-buf
  import, another path; and !13217 (2021), a cleanup of the MIT-SHM paths
  that left the detach out.
- **main at 70bd17c2 (2026-10-15)**: `wsi_common_x11.c` names
  `xcb_shm_detach` nowhere, `x11_image_finish()` still ends with `shmdt`
  alone, and `x11_present_to_x11_sw()` still sends `row_pitches[0] / 4`
  pixels a row. Both are ready to send, which is the account's to do.
- **Read again 2026-10-31**, main at 3d167c08, `mesa-26.2.6` and
  `mesa-26.3.0-rc3`: both faults in all three (`x11_image_finish()` at
  main's line 2844, `x11_present_to_x11_sw()` at 1910), and still nothing
  on the tracker. The file has changed since (!45227, !45076, e102ea9f,
  3b4bc36e), none of it near either.
