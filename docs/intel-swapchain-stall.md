# Draft: a report to Intel

A draft for Intel's graphics driver support, not yet sent. Everything in it
is from DESIGN.md *Measured on Windows itself*, where the measurements are,
and from `intel-swapchain-stall-stacks.txt` beside this file; what is missing
before it can go is marked **owed**, at the end.

## Summary

`vkCreateSwapchainKHR` or `vkDestroySwapchainKHR` sometimes does not return
when a swapchain is replaced while a present is still queued for the
display. The calling thread waits on a thread of the driver's own, which
waits in `D3DKMTWaitForSynchronizationObjectFromCpu`, and stays there: the
window stops drawing and the process does not end. No error is returned and
nothing is logged.

## Where

| | first machine | second machine |
|---|---|---|
| device | Intel HD Graphics 630 | Intel HD Graphics 530 |
| driver | 31.0.101.2145 | 31.0.101.2111 |
| seen | once, and one start in five when the window opens on a monitor of another scale | about one start in twenty |
| Windows | 11, build 26300 (Insider) | 11, build 26300 (Insider) |

Both are hybrid laptops with an NVIDIA device beside the Intel one; the
program was on the Intel device, which owns the panel. The NVIDIA devices
(GTX 1050 Ti, GTX 960M, driver 582.78) have not shown it.

## What the program does

A Vulkan 1.x windowed application, one swapchain, `VK_PRESENT_MODE_FIFO_KHR`,
two images, the driver's minimum. Rendering and every swapchain call are on
one thread; the window's messages are on another. When the window's size
changes, that thread creates a new swapchain with the old one passed as
`oldSwapchain`, and destroys the old one afterwards. The two calls are never
made at the same time: a destroy that overlaps the next create crashes
inside the driver (`0xC0000374` or `0xC0000005`, four runs of twelve at 250
sizes a second), which is a second thing and may be the same one.

## What happens

The first replacement of a swapchain after a pause -- so one with a present
still queued for the display -- enters `vkCreateSwapchainKHR` or
`vkDestroySwapchainKHR` and does not return. It happens within the first
few resizes of a window, or not at all in that run. Twelve of some two
hundred and sixty starts, without the workaround below. The pause matters:
on the HD 530, none of eighty starts stopped when the first resize came a
third of a second after the window opened, and the thirty-fifth did when
it came after four seconds.

Read off the stopped process, every thread's stack. One capture is
attached, `intel-swapchain-stall-stacks.txt`, from the HD 530 on Windows
build 26300.9550; in it:

- the rendering thread is in `vkCreateSwapchainKHR`: `vulkan-1`, then five
  frames of `igvk64`, then a wait in `KERNELBASE`;
- a thread of the driver's (9392 there) is six frames into `igvk64` and
  eleven into `ig9icd64`, and waits in `ntdll` on what reads as a lock;
- another (10520) goes from `igvk64` through `ig9icd64` into a `win32u`
  system call, and a third (6020) from `ig9icd64` into `dwmapi`;
- the window's thread, which had asked for a new size with `SetWindowPos`,
  is inside `ig9icd64` by way of a window hook (`USER32`, then `ig9icd64`
  `+5bc25d`), waiting in `ntdll` on the same four frames as the driver's
  own thread.

So two threads wait on a lock inside `ig9icd64`, one of them the driver's
and one the application's by way of the driver's hook, while the call that
would end it waits for the driver.

## What prevents it

Letting the swapchain come to rest before it is replaced: `vkQueueWaitIdle`
and then 20 ms, one refresh and some, before the create -- only for the
first replacement after a quarter of a second without one. None of ninety
starts stopped with that, and none of ninety with the rest before every
replacement.

Four other arrangements made no difference: keeping the window's thread out
of a resize while a frame is in the driver; making the first swapchain on
the rendering thread; pumping every posted message during the waits; and
pumping only the application's.

## What is asked

Whether replacing a swapchain with a present outstanding is expected to be
safe on this driver, as the specification has it, and if so what the
driver's thread is waiting for.

## Owed before this is sent

- **A second capture, and one from the HD 630.** The one attached is a
  single stop on one machine. The offsets in `igvk64` and `ig9icd64` are
  what Intel can read; whether the same ones come up twice is what would
  make them worth reading.
- **A release build of Windows -- Windows 10, for the HD 630.** Both
  machines are on an Insider build, and a report against one is the first
  thing to be sent back. Intel's own matrix makes it narrower (release
  notes of 31.0.101.2141, read 2026-10-31): 7th-generation Kaby Lake, the
  HD 630, is listed for Windows 10 alone, 20H2 to 22H2, with no Windows 11
  column, and the 6th generation is not in the table at all. So Windows
  10 22H2 is the configuration Intel lists as supported, and leaving the
  Insider build means reinstalling, which wants hands. Intel will likely
  ask for an Intel System Support Utility report as well.
- **A program small enough to attach.** `fenestal-pace` is the library and
  nothing else, but it is not twenty lines of Vulkan; a reproduction that
  is only the two calls would be taken more seriously.
- **Whether a newer driver has it**: answered, neither has one.
  31.0.101.2111 is the last package for the sixth generation, and the HD
  630's 31.0.101.2145 is the latest Intel lists for the 7th to 10th
  (2026-09-22); Intel has taken the ones before 2141 off its download
  centre. Its legacy releases carry no list of known or fixed issues, and
  its community forum has nothing matching (2026-10-31: the nearest are
  a slow `vkCreateSwapchainKHR` on a UHD 770 and old crashes).
