# Draft: X11 swapchains stay VK_ERROR_OUT_OF_DATE_KHR after a resize

A draft for NVIDIA's Linux forum (developer.nvidia.com, *Linux* category),
not yet sent. It goes with `tools/probe/wsidrag.c`, which reproduces it
using Vulkan and xcb and nothing else, and wants `nvidia-bug-report.log.gz`
from the machine attached, taken right after the probe has stuck. Seen with
580.178.04 (the legacy branch, Arch `nvidia-580xx-dkms` and `-utils`) on a
GeForce GTX 1050 Ti Mobile (GP107M) in a Kaby Lake laptop, Linux 7.2.10,
Xorg 21.1.25 with no window manager. The X screen is the Intel GPU's
(modesetting), and the NVIDIA GPU presents to it through PRIME. The
library's workaround is in DESIGN *X11: xcb, not Xlib* (`stale-swapchain`).

Read again 2026-10-31: nothing on NVIDIA's forum (out of date,
`OUT_OF_DATE`, `currentExtent`, swapchain, resize, and the 580 feedback
thread) or in the 580.196.07 notes matches. Nearest: a thread on present
fences after `OUT_OF_DATE` (610.57.04, X11, no swapchain rebuilt) and
Proton #9719 (PRIME, 590.48, `OUT_OF_DATE` repeated, closed as wine's),
the same PRIME, X11 and NVIDIA. schenker's stick now has 580.196.07, so
`wsidrag` there before sending, the version named the one it still
sticks on.

## Summary

After a window is resized repeatedly, with a swapchain rebuilt at each
new size, the driver can get into a state where **every** swapchain made
for the window is out of date at its first `vkAcquireNextImageKHR`. Each
of those swapchains is created at the extent that
`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` reports as `currentExtent`.
The X server agrees about that size, and the window is no longer changing.
It does not recover: 21 rebuilds in 0.4 s, and the same after that for as
long as it was left. Nothing done on the Vulkan side frees it. Changing
the window's size does.

## Reproducing it

    gcc -O2 -Wall -Wextra -pthread -o wsidrag wsidrag.c -lvulkan -lxcb
    FNL_GPU=nvidia ./wsidrag 20

The probe opens a 900x600 window and a second thread resizes it in small
steps: 160x120 larger in 12 steps, then eight drags of 40 pixels each way
in 6 steps, one step every 16 ms, then holds still for a second. That is
one round. The render thread clears and presents every frame. It rebuilds
the swapchain whenever the size from the latest `ConfigureNotify` changes,
passing the old swapchain as `oldSwapchain` and destroying it two frames
later. The probe counts the swapchain as *stuck* after 40 rebuilds in a
row whose first acquire returned `VK_ERROR_OUT_OF_DATE_KHR`. It then
tries each of the escapes listed below and reports which one got an image.
It exits 3 when it stuck and nothing freed it.

On this machine it sticks in 1 to 4 rounds of 20. The full program it was
found in stuck in 7 of 13 of its own test runs.

## What does not free it

Each tried for ten rebuilds, once stuck:

- more rebuilds at the same size;
- a round trip to the X server and 100 ms;
- `vkDeviceWaitIdle`, with every retired swapchain destroyed;
- as above, with the live swapchain destroyed too and no `oldSwapchain`;
- as above, on a new `VkSurfaceKHR` for the same window;
- as above, with half a second in which no swapchain exists;
- as above, on a new X connection;
- a `ConfigureWindow` at the size the window already has.

## What frees it

The window made one pixel wider: an image comes from the first swapchain
made after it, 7 times of 7. One pixel wider and straight back, both
requests sent together, also works, 4 times of 4.

## What does not stick

- The same probe on the Intel GPU (ANV) of the same machine: never in
  20 rounds.
- NVK on the same GPU and the same Xorg.
- This driver under weston (Wayland).
- The probe rebuilding only when told out of date, at `currentExtent`
  rather than the `ConfigureNotify` size: never in 60 rounds. The rebuild
  at each new size, while the window is still moving, seems to be what
  starts it.

## Asked of NVIDIA

Whether the swapchain should ever be out of date at the surface's own
`currentExtent` once the window has stopped changing. If it should not,
what state survives a new surface and a new X connection and is cleared
only by a change of the window's size.

## Aside: two blocks never freed

Under AddressSanitizer, with the ICD kept loaded until exit, the driver
leaves two kinds of allocation behind. Both are small and both are at
exit, not growing over time:

- the D-Bus connection that `vkCreateInstance` opens
  (`dbus_bus_register` under `libnvidia-glcore`);
- one or two Present special events from `xcb_poll_for_special_event` /
  `xcb_wait_for_special_event` under `vkQueuePresentKHR`, 44 bytes each,
  still unfreed when the swapchain is destroyed.

`lsan.supp` in this tree suppresses both.
