#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Does the src/ split actually hold?
#
# The claimed shape: core/ knows no backend and no renderer, backend/<b>/
# knows exactly one windowing system -- or, for kms, the kernel's display
# and input interfaces -- and no graphics API, and renderer/<r>/ knows
# exactly one graphics API and no windowing system. Anything
# crossing those lines goes through the two vtables -- FnlBackendVtbl in
# backend.h, FnlRendererVtbl in renderer.h.
#
# Neither the compiler nor the linker objects when it does not hold: a build
# with one backend configured links fine with core/ reaching straight into
# it. So this counts the references and prints a table.
#
# Run it from anywhere:  python3 tools/check-layering.py
# Exit status is 1 when a forbidden reference survives, so it can gate a commit.
#
# ---- what counts as a reference -------------------------------------------
#
# Comments are stripped first, since the tree's prose names the APIs it
# forbids, and includes with them: backend.h names every backend.
#
# Objective-C is read as C is: its comments are C's, and its #imports are
# includes of another spelling.
#
# Two prefixes are ambiguous and are *not* silently resolved. VK_ is Vulkan's
# constants and also Win32's virtual keys; WM_ is Win32's window messages and
# also the ICCCM atoms every X11 client interns by name. So the APIs are
# matched on what nothing else spells, and the two prefixes are counted into
# columns of their own that are reported and never fail the check.

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Each API, and the pattern that finds it once comments and includes are gone.
APIS = {
    'wayland': r'\b(?:wl|xdg|wp|zwp|zxdg)_\w+',
    'x11':     r'\b(?:xcb_\w+|XCB_[A-Z]\w*)\b',
    'win32':   r'\b(?:HWND|HINSTANCE|WNDCLASS\w*|LRESULT|WPARAM|LPARAM|MSG|'
               r'WS_\w+|CreateWindowEx\w*|DefWindowProc\w*|'
               r'GetMessage\w*|DispatchMessage\w*|MapVirtualKey\w*)\b',
    'vulkan':  r'\b(?:Vk[A-Z]\w*|vk[A-Z]\w*)\b',
    # OpenGL and its bridge, EGL. GL_ and EGL_ are spelled by nothing else
    # in the tree.
    'gl':      r'\b(?:GL_[A-Z]\w*|EGL_\w+|egl[A-Z]\w*|'
               r'GL[a-z]\w*|EGL[A-Z][a-z]\w*|CGL\w+|kCGL\w+)\b',
    # Direct3D 12 and DXGI, by their interfaces, their constants and their
    # entry points, none of which anything else spells.
    'd3d12':   r'\b(?:ID3D12\w+|IDXGI\w+|D3D12\w*|DXGI_\w+|D3D_\w+|'
               r'CreateDXGIFactory\w*)\b',
    # The kernel's own interfaces, which the bare display speaks instead of
    # a window server's: the DRM node and evdev.
    'kms':     r'\b(?:drm_\w+|DRM_[A-Z]\w*|EVIOC\w+|input_event)\b',
    # AppKit, by its own classes and the event calls beside them -- and not
    # by NS alone: NSString and NSError are Foundation's, which Metal speaks
    # as much as AppKit does.
    'cocoa':   r'\b(?:NS(?:Window|View|Event|App|Application|Screen|Cursor|'
               r'Pasteboard|Menu|TrackingArea|TextInput|Color|Workspace|'
               r'Responder)\w*|CGEvent\w*|CGWarp\w*|CGAssociate\w*)\b',
    # Metal, and the layer it presents through, which nothing else spells.
    'metal':   r'\b(?:MTL\w+|CAMetal\w+)\b',
}

# Counted and printed, never fatal. See the note above.
AMBIGUOUS = {
    'VK_?': r'\bVK_[A-Z]\w*',
    'WM_?': r'\bWM_[A-Z]\w*',
}

# Every directory, and the one API it is allowed to name. core/ is allowed
# *none*, with no file excused: each backend's state and each renderer's
# are behind an opaque pointer of App's.
ALLOWED = {
    'src/core':             None,
    # What the Linux backends share and none owns, so it can name none.
    'src/backend':          None,
    'src/backend/wayland':  'wayland',
    'src/backend/x11':      'x11',
    'src/backend/win32':    'win32',
    'src/backend/kms':      'kms',
    'src/backend/cocoa':    'cocoa',
    'src/renderer/vulkan':  'vulkan',
    'src/renderer/gl':      'gl',
    'src/renderer/d3d12':   'd3d12',
    'src/renderer/metal':   'metal',
}

# (directory, api) -> files allowed to name that api anyway, each one argued
# here.
#
# egl_wayland.c is the OpenGL renderer's bridge to Wayland, and EGL takes no
# wl_surface: its native window is a wl_egl_window, which only
# libwayland-egl's own calls make, size and destroy. The Vulkan bridges name
# no window server because vkCreateWaylandSurfaceKHR takes the wl_surface as
# it is; this one cannot.
#
# cocoa.m names the CAMetalLayer and nothing else of Metal's: it is the
# layer the window's view hosts, made with the view, and the one both
# graphics APIs a Mac has present into -- Vulkan through MoltenVK as well --
# so it is the window's, as a Wayland surface is, and the bridges take it
# from there.
#
# cgl.m is the OpenGL renderer's bridge on macOS, and the Mac's window has
# one way in for frames, its Metal layer: the frame drawn into an IOSurface
# is copied into the layer's drawable by Metal, so the bridge names Metal as
# egl_wayland.c names Wayland, and no other file of the renderer does.
#
# present_dxgi.c is the Vulkan renderer's way to a DXGI swapchain on Win32,
# where a swapchain of Vulkan's own is made anew at every size and Intel's
# driver is slow at it: the frames are drawn into images shared with
# Direct3D 11 and presented by it, so it names DXGI as a bridge names a
# window server, and no other file of the renderer does.
EXCUSED: dict[tuple[str, str], set[str]] = {
    ('src/renderer/gl', 'wayland'): {'egl_wayland.c'},
    ('src/renderer/gl', 'metal'): {'cgl.m'},
    ('src/renderer/vulkan', 'd3d12'): {'present_dxgi.c'},
    ('src/backend/cocoa', 'metal'): {'cocoa.m'},
}


def strip(text):
    """Comments and #includes out; string literals left alone.

    None of the APIS patterns appears inside one, which is what makes
    that safe. The AMBIGUOUS ones do -- x11.c interns its ICCCM atoms by
    name -- and those columns are never fatal, so the over-count is free."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'^[ \t]*#[ \t]*(?:include|import).*$', '', text,
                  flags=re.M)


def main():
    cols = list(APIS)
    amb = list(AMBIGUOUS)
    print(f"{'directory':<24}" + ''.join(f'{c:>9}' for c in cols)
          + ''.join(f'{c:>9}' for c in amb))
    print('-' * (24 + 9 * (len(cols) + len(amb))))

    failures = []
    for d, allowed in ALLOWED.items():
        path = os.path.join(ROOT, d)
        if not os.path.isdir(path):
            continue
        files = sorted(f for f in os.listdir(path)
                       if f.endswith(('.c', '.h', '.m')))
        counts = {c: 0 for c in cols}
        ambiguous = {c: 0 for c in amb}
        for f in files:
            with open(os.path.join(path, f), encoding='utf-8') as fh:
                src = strip(fh.read())
            for name, pat in AMBIGUOUS.items():
                ambiguous[name] += len(re.findall(pat, src))
            for api, pat in APIS.items():
                n = len(re.findall(pat, src))
                counts[api] += n
                if n and api != allowed and f not in EXCUSED.get((d, api), ()):
                    failures.append((d, f, api, n))

        print(f'{d:<24}' + ''.join(f'{counts[c]:>9}' for c in cols)
              + ''.join(f'{ambiguous[c]:>9}' for c in amb))

    print()
    if not failures:
        print('layering holds: no directory names an API it should not.')
        return 0
    print('LAYERING VIOLATIONS')
    for d, f, api, n in failures:
        print(f'  {d}/{f}: {n} {api} reference{"s" if n > 1 else ""}')
    return 1


if __name__ == '__main__':
    sys.exit(main())
