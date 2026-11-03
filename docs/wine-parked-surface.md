# Draft: wine keeps a destroyed Vulkan surface's frame over a child window

A draft for wine's Bugzilla (product Wine, component winewayland.drv, or
else win32u; version 11.20), not yet sent. Met in the stress round under
wine 11.20's Wayland driver on weston 15.0.1 and cage 0.3.1, on all three
bare machines (DESIGN *Under wine on Wayland the child is there from the
start*), and read in master on 2026-10-31, after 11.21: the code below is
the same there. The library steps round it under wine's Wayland driver by
presenting into a child from the window's first frame, so the window
itself never has a surface to park.

`tools/probe/parkedchild.c` is the reproducer, written for this and **not
yet built or run**: what it should show is read from the source and from
the round's measurements, not yet seen through it. To attach once it has
been: the probe, and a capture of each mode.

## Summary

winewayland.drv: a child window's Vulkan frames are hidden under the
parent's last frame once the parent's VkSurfaceKHR has been destroyed

## Description

A program presents into its top-level window through Vulkan, then
destroys the swapchain and the surface and presents into a child window
filling the client area instead (as a program does that moves to another
GPU and wants a window no driver has claimed). On Windows the child's
frames are shown. Under winewayland.drv the window goes on showing the
top-level's last frame until it is destroyed, and the child's frames are
underneath it.

Two things combine:

1. `win32u_vkDestroySurfaceKHR()` (`dlls/win32u/vulkan.c`, master line
   1595) calls `use_window_client_surface(surface->client, FALSE)`, which
   (`dlls/win32u/window.c`, line 490) moves the client surface to
   `unused_surfaces` and takes a reference, keeping it for the window's
   next surface of the same format. It stays attached: its `hwnd` is
   kept and nothing calls the driver's `detach`, so on Wayland its
   subsurface stays mapped with the last buffer presented. It is released
   only when a later surface on the same window reuses it
   (`get_unused_client_surface()`, line 514) or the window is destroyed
   (`detach_client_surfaces()`). This is !11351 ("win32u: Keep unused
   client surfaces around and reuse them if possible", merged
   2026-08-10), so that a VkSurfaceKHR made again after a
   `VK_SUBOPTIMAL_KHR` finds its wl_surface initialised.
2. `wayland_surface_reconfigure_client()`
   (`dlls/winewayland.drv/wayland_surface.c`, line 791) places every client
   subsurface with `wl_subsurface_place_above(client->wl_subsurface,
   surface->wl_surface)` -- directly above the top-level's own surface,
   which is below every client subsurface already placed above it. So a
   child window's client subsurface, made after the parked one, is
   stacked under it.

On weston the program goes on drawing and the window shows the frozen
frame. On cage (wlroots) it stops: wlroots sends no frame callback to a
surface that is covered, so the child's swapchain blocked after five
presents; wine was still delivering pointer input to the window
(`WM_MOUSEMOVE` on the top-level, traced with `+msg`), but the program,
waiting on its next frame, never read it.

## Steps to reproduce

parkedchild.c (attached) clears its window red for a second, destroys
the swapchain and the surface, makes a child window over the whole client
area and clears that green for ten seconds; `parkedchild self` presents
green into the window again instead, the control. Build:

    x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wextra -mconsole \
        -I<vulkan headers> -o parkedchild.exe parkedchild.c \
        -lvulkan-1 -luser32

1. Start a Wayland compositor (weston). Run with no DISPLAY, so that wine
   uses winewayland.drv:

       wine parkedchild.exe

2. Look at the window after the line "child: green".

Expected: green, as on Windows.
Actual (expected from the source and the round; to be confirmed with the
probe): red until the program exits. With `self`: green, the parked
surface reused.

## Possible fixes

Either would do, and neither undoes what !11351 is for; the first matches
Windows more closely.

- Stack a child window's client subsurface above its parent's, in the
  windows' Z order, rather than every client subsurface directly above
  the top-level's own surface.
- Keep a parked client surface's wl_surface but unmap it (commit no
  buffer) until it is reused, so a parked surface shows nothing.

## Owed before it is sent

- `parkedchild` built and run on weston and on cage, under 11.20 and the
  current development release, both modes, with a capture of each; and
  on Windows, for the expected result.

Searched 2026-10-31: Bugzilla ("winewayland subsurface", "client surface
vulkan child", "wayland child window vulkan", "vulkan child window",
"winewayland vulkan frozen", "wayland subsurface stacking") and wine's
merge requests ("client surface"). Nothing matches. The nearest are
45277 (Vulkan child window rendering in general, winevulkan), 58547 (a
2025 winewayland regression in Roblox Studio, from another commit) and
59398 (Vulkan children and the parent's GDI clip region), and !11342
("winewayland: Reuse a single client surface for vulkan"), closed.
