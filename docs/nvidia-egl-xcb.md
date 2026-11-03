# Draft: NVIDIA's EGL lists EGL_EXT_platform_xcb and refuses every xcb display

A draft for NVIDIA's egl-x11 issue tracker (github.com/NVIDIA/egl-x11),
not yet sent. Seen 2026-10-28 on schenker, a laptop with an Intel HD 630
driving its panel and a GeForce GTX 1050 Ti beside it, on Arch Linux:
NVIDIA 580.196.07 (`nvidia-580xx-utils`), egl-x11 1.0.6, libglvnd 1.7.0,
libxcb 1.17.0, xorg-server 21.1.25, Mesa 26.2.5, Linux 7.2.11. This
library reaches X11 through xcb alone and links no Xlib, so NVIDIA's
OpenGL never draws one of its X11 windows (DESIGN *What a frame costs,
compared*).

## Summary

With the NVIDIA vendor library as glvnd's only EGL vendor, the client
extensions include `EGL_EXT_platform_xcb`, and
`eglGetPlatformDisplay(EGL_PLATFORM_XCB_EXT, conn, ...)` returns
`EGL_NO_DISPLAY` with `eglGetError()` at `EGL_SUCCESS`, on every X
server tried, with or without `EGL_PLATFORM_XCB_SCREEN_EXT`. In the same
process, on the same server, with the same environment,
`eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, dpy, ...)` returns a
display that initializes, vendor `NVIDIA`.

Under glvnd's own choice of vendor the xcb request then falls through to
Mesa, so on a PRIME render offload desktop an xcb client that asks for
the NVIDIA GPU (`__NV_PRIME_RENDER_OFFLOAD=1`) is given the Intel GPU
without a word, where an Xlib client is given NVIDIA's.

## Reproduction

    /* cc -o eglxcb eglxcb.c -lEGL -lxcb -lX11 */
    #include <EGL/egl.h>
    #include <EGL/eglext.h>
    #include <X11/Xlib.h>
    #include <xcb/xcb.h>
    #include <stdio.h>

    static void try(const char *what, EGLenum platform, void *native,
                    const EGLAttrib *attrs)
    {
        EGLDisplay d = eglGetPlatformDisplay(platform, native, attrs);
        EGLint err = eglGetError();
        EGLBoolean ok = d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL);
        printf("%-5s eglGetPlatformDisplay %s (error 0x%04x), "
               "eglInitialize %s, vendor %s\n", what,
               d == EGL_NO_DISPLAY ? "EGL_NO_DISPLAY" : "a display", err,
               ok ? "ok" : "-", ok ? eglQueryString(d, EGL_VENDOR) : "-");
    }

    int main(void)
    {
        const char *ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        printf("client extensions: %s\n\n", ext ? ext : "(none)");

        Display *xlib = XOpenDisplay(NULL);
        const EGLAttrib xlib_attrs[] = { EGL_PLATFORM_X11_SCREEN_KHR, 0,
                                         EGL_NONE };
        try("xlib", EGL_PLATFORM_X11_KHR, xlib, xlib_attrs);

        int screen = 0;
        xcb_connection_t *xcb = xcb_connect(NULL, &screen);
        const EGLAttrib xcb_attrs[] = { EGL_PLATFORM_XCB_SCREEN_EXT, screen,
                                        EGL_NONE };
        try("xcb", EGL_PLATFORM_XCB_EXT, xcb, xcb_attrs);
        try("xcb", EGL_PLATFORM_XCB_EXT, xcb, NULL);
        return 0;
    }

Run with `__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json`
to take Mesa out of the picture, and without it to see what an
application meets.

## What it gives

**The Intel GPU's X server** (modesetting, `NVIDIA-G0` as its GPU
screen), `__NV_PRIME_RENDER_OFFLOAD=1`, the NVIDIA vendor alone:

    client extensions: EGL_EXT_platform_base EGL_EXT_device_base
      EGL_KHR_client_get_all_proc_addresses EGL_EXT_client_extensions
      EGL_KHR_debug EGL_KHR_platform_x11 EGL_EXT_platform_x11
      EGL_EXT_platform_device EGL_MESA_platform_surfaceless
      EGL_EXT_explicit_device EGL_KHR_platform_wayland
      EGL_EXT_platform_wayland EGL_KHR_platform_gbm EGL_MESA_platform_gbm
      EGL_EXT_platform_xcb

    xlib  eglGetPlatformDisplay a display (error 0x3000), eglInitialize ok, vendor NVIDIA
    xcb   eglGetPlatformDisplay EGL_NO_DISPLAY (error 0x3000), eglInitialize -, vendor -
    xcb   eglGetPlatformDisplay EGL_NO_DISPLAY (error 0x3000), eglInitialize -, vendor -

The same, both vendors (glvnd choosing):

    xlib  eglGetPlatformDisplay a display (error 0x3000), eglInitialize ok, vendor NVIDIA
    xcb   eglGetPlatformDisplay a display (error 0x3000), eglInitialize ok, vendor Mesa Project
    xcb   eglGetPlatformDisplay a display (error 0x3000), eglInitialize ok, vendor Mesa Project

Without `__NV_PRIME_RENDER_OFFLOAD`, both refused under the NVIDIA vendor
alone, which is expected for a server on another GPU.

**An X server NVIDIA's driver drives** (the NVIDIA screen primary, the
Intel panel as a PRIME output source), both vendors, and again with
`__NV_FORCE_ENABLE_X11_EGL_PLATFORM=1` -- the same lines each time:

    xlib  eglGetPlatformDisplay a display (error 0x3000), eglInitialize ok, vendor NVIDIA
    xcb   eglGetPlatformDisplay a display (error 0x3000), eglInitialize ok, vendor Mesa Project
    xcb   eglGetPlatformDisplay a display (error 0x3000), eglInitialize ok, vendor Mesa Project

Mesa there is its software rasteriser, having no driver for the NVIDIA
device (`failed to create dri2 screen`).

The order of the two requests does not matter: xcb first or Xlib first,
the xcb one is refused. The connection is a local Unix socket, and the
server has DRI3 and Present.

## Expected

Either an xcb display wherever the Xlib one is given -- egl-x11's source
reaches both through one function, `eplX11GetPlatformDisplay()`, the
Xlib display converted to its xcb connection -- or, where xcb is not
served, no `EGL_EXT_platform_xcb` in the client extensions, and failing
that an error from `eglGetPlatformDisplay` saying why. Silence makes
glvnd hand the display to the next vendor, which is how an offload
request ends on the wrong GPU.

## Read in the source, for whoever takes it

egl-x11's `CheckServerExtensions()` (`src/x11/x11-platform.c`, main at
cad025e, 2026-10-29) declines a server with `NV-GLX` unless
`__NV_FORCE_ENABLE_X11_EGL_PLATFORM` is set, so that the driver's own X11
path takes it, and declines one whose DRI3 or Present is older than 1.2,
whatever the variable; `eplX11DisplayInstanceCreate()` says why only when
called from `eglInitialize` (`from_init`), never from
`eglGetPlatformDisplay`. Those silent paths may be the whole of it:

- On the Intel GPU's server under offload, NVIDIA's GLX module serves
  `NVIDIA-G0` and may put `NV-GLX` in the server's extension list, which
  declines it there too; the variable was not tried on that server. The
  Xlib display that is given would then be the driver's own X11 path,
  which takes Xlib alone, and not egl-x11's.
- On the server NVIDIA's driver drives, with the variable, a DRI3 below
  1.2 would decline it: egl-x11's PR #32 (December 2025) called DRI3 1.2
  in NVIDIA's Xorg module "upcoming", and this is the 580 legacy branch.

If so, the report is narrower than it reads above: an extension listed
for a path that declines the server in silence. Owed on schenker before
it is sent, each a line in the results above:

1. `xdpyinfo -queryExtensions` on the Intel GPU's server under offload:
   is `NV-GLX` there?
2. The DRI3 and Present versions the NVIDIA-driven server answers.
3. The Intel server under offload with `__NV_FORCE_ENABLE_X11_EGL_PLATFORM=1`.
4. The Xlib display with `20_nvidia_xlib.json` taken out of
   `/usr/share/egl/egl_external_platform.d/`: still NVIDIA's, then it was
   never egl-x11's.

## Elsewhere, read 2026-10-31

No issue or pull request on github.com/NVIDIA/egl-x11 has this, open or
closed; nothing since v1.0.6 (2026-09-04, the latest tag) touches the xcb
display's choice. NVIDIA's forum has the symptom on an NVIDIA-driven
server, with no reply: "Cannot EGL working on X11 using XCB with
getPlatformDisplayEXT" (2026-05-30, 580.159.04, GTX 1080), xcb given Mesa
and Xlib NVIDIA -- worth citing. egl-x11 #39 (closed) is where its
maintainer sets out when it steps aside for Mesa on a server not
NVIDIA's.
