---
name: capture
description: Capture a frame from the Fenestal testbed through the Mesa screenshot layer and read pixel values off it correctly. Use when asked to screenshot the app, grab a frame, or check a colour/palette change against real pixels.
disable-model-invocation: true
---

Capture a frame and, if the user is comparing values, interpret it correctly.
The interpretation is the part that goes wrong.

The obvious routes do not work here: GNOME answers `org.gnome.Shell.Screenshot`
with `AccessDenied` for an ordinary client, and X11 grabbers (`import`, `scrot`)
cannot see a native Wayland surface at all. Mesa's screenshot layer takes the
image out of the swapchain, which is also the right place to look — it is the
buffer the client produced, with no compositor in between.

The X11 backend (`FNL_BACKEND=x11` on the `linux` tree) is the
exception and is worth reaching for when the
question is about *layout* rather than about colour: it is a real X11 window on
XWayland, so `import -window <id>` captures it directly and
`tools/drive/x11_driver.py`'s `capture()` is one call. That path holds
sRGB-encoded bytes like any PNG a grabber writes, so point 2 below does not
apply to it — and it goes through the compositor's copy rather than the
swapchain's, so point 2 is the reason to keep using the layer for anything
about colour.

```sh
VK_LOADER_LAYERS_ENABLE=VK_LAYER_MESA_screenshot \
VK_LAYER_MESA_SCREENSHOT_CONFIG="frames=150,output_dir=/tmp/shots" \
FNL_BACKEND=wayland \
    ./build/linux/release/fenestal-testbed
```

`frames=` takes a list, `all`, or `<start>-<count>-<interval>`; the file is named
for the frame number.

## Three things that will otherwise mislead you

1. **It does not compose with the validation layer.** Enable both and no file is
   written. Capture and validate in separate runs.
2. **The PNG holds linear light, not sRGB.** On an `_SRGB` surface the swapchain
   bytes are encoded and the layer hands back those values decoded. The
   titlebar, authored as sRGB 41, reads **6**. Always compare against
   `to_linear()` of the authored value — otherwise a correct frame looks badly
   wrong.
3. **Leave the scene's clock stopped.** It is stopped at startup and `F2` starts
   it, so `scene_time == 0` is the default and is the same frame on every run and
   every machine. Any moving frame is not reproducible; do not capture one and
   then compare it against a stored reference.

## The check to repeat after touching colour

After changing the palette, the surface format preference, or anything in
`testbed/shaders/scene.frag`, run the two-surface-format comparison:
**both formats must produce the same picture.**

Capture the frozen frame normally, then force the UNORM path by making the
`_SRGB` preference loop in `query_surface_info()` fail (put a `_UNORM` format
first in `SRGB_FORMATS[]`) and capture again. The first PNG holds linear light
and the second holds sRGB-encoded bytes — **encode the first before
comparing.**

Near black the two can only agree to within how coarse 8-bit linear is there,
so expect a small non-zero RMSE over the scene rather than an exact match; that
floor is the capture format's, not a mismatch. The titlebar differs by more on
purpose when `dualSrcBlend` is absent: the text pipeline has its own grayscale
fallback, which is a different picture by design.

If a change lands anywhere near 25/255 RMSE, the linear/sRGB handling is wrong,
not the palette.

## Fallback paths

They never run on hardware that has the feature, so each is forced by one edit.
`DESIGN.md` *Forcing the paths that never run* lists them with the expected
result — check there before hand-rolling a scratch build.
