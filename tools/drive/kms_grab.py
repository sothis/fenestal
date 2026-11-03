# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""What the bare machine's display is scanning out, as a PPM on stdout.

Run *there*, as root, by bare_driver.Bare.kms_capture() -- never on this
desktop. With no compositor there is nobody to ask for a picture, so this
asks the kernel: the CRTC driving the connector `--output` names (eDP-1,
HDMI-A-1: the kernel's names, as the KMS backend logs them), or else the
first active CRTC with a framebuffer on it, on whichever card has one,
that framebuffer mapped and read out of its layout (linear,
or Intel's X or Y tiling, which the display path's own images have), and
the cursor plane on that CRTC blended over it where the plane is. Root
because DRM_IOCTL_MODE_GETFB hands another client's buffer to
CAP_SYS_ADMIN alone. A picture of the screen and not of the program's
frame: the KMS backend's copy path or display path, the mode set, the flips
and the pointer are all in it, which is what makes it the round's
"composited".

Standard library only, the ioctls by hand: the machine has python3 and
nothing of this tree but this file.
"""

import ctypes
import errno
import fcntl
import glob
import mmap
import os
import struct
import sys

# <drm/drm.h> and <drm/drm_mode.h>, read off the headers' own sizes.
GETRESOURCES = 0xC04064A0       # struct drm_mode_card_res, 64
GETCRTC = 0xC06864A1            # struct drm_mode_crtc, 104
GETENCODER = 0xC01464A6         # struct drm_mode_get_encoder, 20
GETCONNECTOR = 0xC05064A7       # struct drm_mode_get_connector, 80
GETFB = 0xC01C64AD              # struct drm_mode_fb_cmd, 28
GETFB2 = 0xC06864CE             # struct drm_mode_fb_cmd2, 104
FB_MODIFIERS = 2                # DRM_MODE_FB_MODIFIERS, in its flags
# <drm/drm_fourcc.h>: the layouts rows() can read.
MOD_LINEAR = 0
MOD_INTEL_X = (1 << 56) | 1     # I915_FORMAT_MOD_X_TILED
MOD_INTEL_Y = (1 << 56) | 2     # I915_FORMAT_MOD_Y_TILED
TILE_ROWS = {MOD_INTEL_X: 8, MOD_INTEL_Y: 32}
MAP_DUMB = 0xC01064B3           # struct drm_mode_map_dumb, 16
GEM_CLOSE = 0x40086409          # struct drm_gem_close, 8
PRIME_HANDLE_TO_FD = 0xC00C642D  # struct drm_prime_handle, 12
DRM_CLOEXEC = os.O_CLOEXEC
# EGL's and GLES 2's enums, for gpu_read(): <EGL/egl.h>, <EGL/eglext.h>,
# <GLES2/gl2.h>.
EGL_PLATFORM_SURFACELESS_MESA = 0x31DD
EGL_OPENGL_ES_API = 0x30A0
EGL_CONTEXT_CLIENT_VERSION = 0x3098
EGL_NONE = 0x3038
EGL_WIDTH, EGL_HEIGHT = 0x3057, 0x3056
EGL_LINUX_DMA_BUF_EXT = 0x3270
EGL_LINUX_DRM_FOURCC_EXT = 0x3271
# Plane n's descriptor, offset and pitch, and its modifier's two halves.
EGL_PLANE_ATTRS = ((0x3272, 0x3273, 0x3274, 0x3443, 0x3444),
                   (0x3275, 0x3276, 0x3277, 0x3445, 0x3446),
                   (0x3278, 0x3279, 0x327A, 0x3447, 0x3448),
                   (0x3440, 0x3441, 0x3442, 0x3449, 0x344A))
GL_TEXTURE_2D = 0x0DE1
GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_NEAREST = \
    0x2801, 0x2800, 0x2600
GL_FRAMEBUFFER, GL_RENDERBUFFER = 0x8D40, 0x8D41
GL_COLOR_ATTACHMENT0 = 0x8CE0
GL_FRAMEBUFFER_COMPLETE = 0x8CD5
GL_RGBA8 = 0x8058
GL_RGBA, GL_UNSIGNED_BYTE = 0x1908, 0x1401
GL_VERTEX_SHADER, GL_FRAGMENT_SHADER = 0x8B31, 0x8B30
GL_COMPILE_STATUS, GL_LINK_STATUS = 0x8B81, 0x8B82
GL_TRIANGLES = 0x0004
# One triangle over the whole target, sampling the image texel for texel:
# its uv is 0 at the target's row 0 and the texture's, which is the
# buffer's first row.
COPY_VS = b"""#version 300 es
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""
COPY_FS = b"""#version 300 es
precision highp float;
uniform sampler2D image;
in vec2 uv;
out vec4 colour;
void main() { colour = texture(image, uv); }
"""
MESA_EGL_VENDOR = "/usr/share/glvnd/egl_vendor.d/50_mesa.json"
SET_CLIENT_CAP = 0x4010640D     # struct drm_set_client_cap, 16
GETPLANERESOURCES = 0xC01064B5  # struct drm_mode_get_plane_res, 16
GETPLANE = 0xC02064B6           # struct drm_mode_get_plane, 32
OBJ_GETPROPERTIES = 0xC02064B9  # struct drm_mode_obj_get_properties, 32
GETPROPERTY = 0xC04064AA        # struct drm_mode_get_property, 64
CAP_ATOMIC = 3                  # DRM_CLIENT_CAP_ATOMIC
OBJECT_PLANE = 0xEEEEEEEE       # DRM_MODE_OBJECT_PLANE
OBJECT_CRTC = 0xCCCCCCCC        # DRM_MODE_OBJECT_CRTC
PLANE_TYPE_CURSOR = 2           # the "type" property's DRM_PLANE_TYPE_CURSOR
MAX_CRTCS = 16
MAX_CONNECTORS = 32
# DRM_MODE_CONNECTOR_*, by the names the kernel gives them.
CONNECTOR_TYPES = ("Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite",
                   "SVIDEO", "LVDS", "Component", "DIN", "DP", "HDMI-A",
                   "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI",
                   "Writeback", "SPI", "USB")
MAX_PLANES = 64
MAX_PROPS = 64


def ioctl(fd, req, buf):
    arg = bytearray(buf)
    fcntl.ioctl(fd, req, arg, True)
    return bytes(arg)


def crtcs(fd):
    ids = bytearray(4 * MAX_CRTCS)
    ptr = ctypes_address(ids)
    res = struct.pack("QQQQIIII16x", 0, ptr, 0, 0, 0, MAX_CRTCS, 0, 0)
    out = ioctl(fd, GETRESOURCES, res)
    n = min(struct.unpack_from("I", out, 36)[0], MAX_CRTCS)
    return struct.unpack_from("%dI" % n, ids)


def connectors(fd):
    ids = bytearray(4 * MAX_CONNECTORS)
    res = struct.pack("QQQQIIII16x", 0, 0, ctypes_address(ids), 0,
                      0, 0, MAX_CONNECTORS, 0)
    out = ioctl(fd, GETRESOURCES, res)
    n = min(struct.unpack_from("I", out, 40)[0], MAX_CONNECTORS)
    return struct.unpack_from("%dI" % n, ids)


def modesetting(fd):
    """Whether this card drives outputs at all. A render-only node -- the
    Pi's v3d, beside the vc4 that drives its HDMI -- refuses GETRESOURCES
    with EOPNOTSUPP, and is passed over."""
    try:
        crtcs(fd)
    except OSError as e:
        if e.errno == errno.EOPNOTSUPP:
            return False
        raise
    return True


def connector_crtc(fd, name):
    """The CRTC driving the connector of that name on this card: 0 when
    the card has it and nothing drives it, None when the card has no
    connector of that name. Asked for one mode, into a scratch buffer:
    with count_modes 0 the kernel probes the connector, which can send the
    program under test a hotplug; with it, the state as it stands."""
    scratch = bytearray(68)     # struct drm_mode_modeinfo
    for cid in connectors(fd):
        got = ioctl(fd, GETCONNECTOR,
                    struct.pack("4Q12I", 0, ctypes_address(scratch), 0, 0,
                                1, 0, 0, 0, cid, 0, 0, 0, 0, 0, 0, 0))
        encoder, _cid, ctype, ctype_id = struct.unpack_from("4I", got, 44)
        kind = (CONNECTOR_TYPES[ctype] if ctype < len(CONNECTOR_TYPES)
                else "Unknown")
        if "%s-%d" % (kind, ctype_id) != name:
            continue
        if not encoder:
            return 0
        enc = ioctl(fd, GETENCODER, struct.pack("5I", encoder, 0, 0, 0, 0))
        return struct.unpack_from("I", enc, 8)[0]
    return None


def ctypes_address(buf):
    return ctypes.addressof((ctypes.c_char * len(buf)).from_buffer(buf))


def fb_info(fd, fb_id):
    """(width, height, pitch, handle, offset) of plane 0. GETFB2 first:
    the old GETFB refuses a framebuffer made with modifiers and planes,
    which is what weston's are."""
    try:
        got = ioctl(fd, GETFB2, struct.pack("I", fb_id) + bytes(100))
        w, h = struct.unpack_from("2I", got, 4)
        handle = struct.unpack_from("I", got, 20)[0]
        pitch = struct.unpack_from("I", got, 36)[0]
        offset = struct.unpack_from("I", got, 52)[0]
        return w, h, pitch, handle, offset
    except OSError:
        fb = ioctl(fd, GETFB, struct.pack("7I", fb_id, 0, 0, 0, 0, 0, 0))
        _id, w, h, pitch, _bpp, _depth, handle = struct.unpack("7I", fb)
        return w, h, pitch, handle, 0


# DRM_FORMAT_RGB565, 'RG16': the 16-bit framebuffer a console such as the
# Pi's vc4 leaves up once the program that set a mode has gone.
FORMAT_RGB565 = 0x36314752


def pixel_bytes(fd, fb_id):
    """Bytes a pixel in the framebuffer: 2 for RGB565, 4 for the 32-bit
    formats every program here scans out."""
    try:
        got = ioctl(fd, GETFB2, struct.pack("I", fb_id) + bytes(100))
        return 2 if struct.unpack_from("I", got, 12)[0] == FORMAT_RGB565 \
            else 4
    except OSError:
        fb = ioctl(fd, GETFB, struct.pack("7I", fb_id, 0, 0, 0, 0, 0, 0))
        return max(1, struct.unpack("7I", fb)[4] // 8)


def mapped(fd, fb_id, tile_rows=1):
    """A framebuffer's width, height, pitch and pixels, mapped; or None.
    A tiled image is whole tiles, `tile_rows` high, so its last tile row
    runs past the height and is mapped with it."""
    w, h, pitch, handle, start = fb_info(fd, fb_id)
    if not handle:
        return None
    md = ioctl(fd, MAP_DUMB, struct.pack("IIQ", handle, 0, 0))
    offset = struct.unpack_from("Q", md, 8)[0]
    if start:
        sys.exit("plane 0 starts %d bytes into its buffer, which this does "
                 "not read" % start)
    tall = -(-h // tile_rows) * tile_rows
    m = mmap.mmap(fd, pitch * tall, mmap.MAP_SHARED, mmap.PROT_READ,
                  offset=offset)
    ioctl(fd, GEM_CLOSE, struct.pack("II", handle, 0))
    return w, h, pitch, m


def gpu_read(fd, fb_id):
    """A framebuffer the CPU may not map, read back by the GPU: (w, h,
    pixels), the pixels linear B, G, R, X rows as a mapped dumb buffer's
    are. amdgpu refuses both MAP_DUMB and the mmap of the dma-buf for an
    image made in VRAM with no CPU access -- RADV's scanout images, and
    mutter's -- so the image is exported, imported into Mesa's EGL as it
    is, modifier and every plane, drawn into an 8-bit framebuffer of the
    script's own and read with glReadPixels. Drawn, not read where it is:
    an imported image need not be one GL renders to -- mutter's 10-bit
    XRGB2101010 scanout is incomplete as an attachment -- and every image
    a driver imports it samples. Mesa's EGL alone, through ctypes and the
    machine's own libraries: NVIDIA's vendor, first in glvnd's order on a
    machine that has one, would be asked about an AMD or Intel buffer."""
    got = ioctl(fd, GETFB2, struct.pack("I", fb_id) + bytes(100))
    w, h, fourcc, flags = struct.unpack_from("4I", got, 4)
    handles = struct.unpack_from("4I", got, 20)
    pitches = struct.unpack_from("4I", got, 36)
    offsets = struct.unpack_from("4I", got, 52)
    mods = struct.unpack_from("4Q", got, 72)
    if not handles[0]:
        sys.exit("no handle for the framebuffer: run this as root")
    fds = []
    for hd in handles:
        if not hd:
            break
        ph = ioctl(fd, PRIME_HANDLE_TO_FD,
                   struct.pack("IIi", hd, DRM_CLOEXEC | os.O_RDWR, -1))
        fds.append(struct.unpack_from("i", ph, 8)[0])
    for hd in set(h_ for h_ in handles if h_):
        ioctl(fd, GEM_CLOSE, struct.pack("II", hd, 0))

    if os.path.exists(MESA_EGL_VENDOR):
        os.environ["__EGL_VENDOR_LIBRARY_FILENAMES"] = MESA_EGL_VENDOR
    egl = ctypes.CDLL("libEGL.so.1")
    gles = ctypes.CDLL("libGLESv2.so.2")
    vp, u, i = ctypes.c_void_p, ctypes.c_uint, ctypes.c_int
    egl.eglGetPlatformDisplay.restype = vp
    egl.eglGetPlatformDisplay.argtypes = [u, vp, vp]
    egl.eglInitialize.argtypes = [vp, vp, vp]
    egl.eglCreateContext.restype = vp
    egl.eglCreateContext.argtypes = [vp, vp, vp, vp]
    egl.eglMakeCurrent.argtypes = [vp, vp, vp, vp]
    egl.eglGetProcAddress.restype = vp
    egl.eglGetProcAddress.argtypes = [ctypes.c_char_p]
    egl.eglDestroyContext.argtypes = [vp, vp]
    egl.eglTerminate.argtypes = [vp]
    egl.eglGetError.restype = i
    create_image = ctypes.CFUNCTYPE(vp, vp, vp, u, vp, vp)(
        egl.eglGetProcAddress(b"eglCreateImageKHR"))
    destroy_image = ctypes.CFUNCTYPE(u, vp, vp)(
        egl.eglGetProcAddress(b"eglDestroyImageKHR"))
    to_texture = ctypes.CFUNCTYPE(None, u, vp)(
        egl.eglGetProcAddress(b"glEGLImageTargetTexture2DOES"))

    dpy = egl.eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, None,
                                    None)
    if not dpy or not egl.eglInitialize(dpy, None, None):
        sys.exit("Mesa's surfaceless EGL display would not open (error "
                 "%#x)" % egl.eglGetError())
    egl.eglBindAPI(EGL_OPENGL_ES_API)
    # GLES 3: a framebuffer over a 10-bit image -- mutter's scanout is
    # XRGB2101010 -- is complete there and not in 2.
    ctx_attrs = (i * 3)(EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE)
    ctx = egl.eglCreateContext(dpy, None, None, ctx_attrs)
    if not ctx or not egl.eglMakeCurrent(dpy, None, None, ctx):
        sys.exit("no GLES 3 context on Mesa's surfaceless display (error "
                 "%#x)" % egl.eglGetError())

    attrs = [EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, fourcc]
    for n, pfd in enumerate(fds):
        a_fd, a_off, a_pitch, a_lo, a_hi = EGL_PLANE_ATTRS[n]
        attrs += [a_fd, pfd, a_off, offsets[n], a_pitch, pitches[n]]
        if flags & FB_MODIFIERS:
            attrs += [a_lo, mods[n] & 0xFFFFFFFF, a_hi, mods[n] >> 32]
    attrs.append(EGL_NONE)
    img = create_image(dpy, None, EGL_LINUX_DMA_BUF_EXT, None,
                       (i * len(attrs))(*[ctypes.c_int(x & 0xFFFFFFFF).value
                                          for x in attrs]))
    for pfd in fds:
        os.close(pfd)
    if not img:
        sys.exit("Mesa's EGL would not import the framebuffer (format "
                 "%#x, modifier %#x, error %#x)"
                 % (fourcc, mods[0], egl.eglGetError()))
    tex, rb, fbo = u(0), u(0), u(0)
    gles.glGenTextures(1, ctypes.byref(tex))
    gles.glBindTexture(GL_TEXTURE_2D, tex)
    to_texture(GL_TEXTURE_2D, img)
    gles.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)
    gles.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST)
    gles.glGenRenderbuffers(1, ctypes.byref(rb))
    gles.glBindRenderbuffer(GL_RENDERBUFFER, rb)
    gles.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h)
    gles.glGenFramebuffers(1, ctypes.byref(fbo))
    gles.glBindFramebuffer(GL_FRAMEBUFFER, fbo)
    gles.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_RENDERBUFFER, rb)
    if gles.glCheckFramebufferStatus(GL_FRAMEBUFFER) != \
            GL_FRAMEBUFFER_COMPLETE:
        sys.exit("no %dx%d framebuffer to draw the image into" % (w, h))
    gles.glCreateShader.restype = u
    gles.glCreateProgram.restype = u

    def shader(kind, src):
        sh = gles.glCreateShader(kind)
        text = ctypes.c_char_p(src)
        gles.glShaderSource(sh, 1, ctypes.byref(text), None)
        gles.glCompileShader(sh)
        ok = i(0)
        gles.glGetShaderiv(sh, GL_COMPILE_STATUS, ctypes.byref(ok))
        if not ok.value:
            sys.exit("the copy shader would not compile")
        return sh
    prog = gles.glCreateProgram()
    gles.glAttachShader(prog, shader(GL_VERTEX_SHADER, COPY_VS))
    gles.glAttachShader(prog, shader(GL_FRAGMENT_SHADER, COPY_FS))
    gles.glLinkProgram(prog)
    ok = i(0)
    gles.glGetProgramiv(prog, GL_LINK_STATUS, ctypes.byref(ok))
    if not ok.value:
        sys.exit("the copy shader would not link")
    gles.glUseProgram(prog)
    gles.glViewport(0, 0, w, h)
    gles.glDrawArrays(GL_TRIANGLES, 0, 3)
    rgba = (ctypes.c_char * (w * h * 4))()
    gles.glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba)
    gles.glDeleteProgram(prog)
    gles.glDeleteFramebuffers(1, ctypes.byref(fbo))
    gles.glDeleteRenderbuffers(1, ctypes.byref(rb))
    gles.glDeleteTextures(1, ctypes.byref(tex))
    destroy_image(dpy, img)
    egl.eglMakeCurrent(dpy, None, None, None)
    egl.eglDestroyContext(dpy, ctx)
    egl.eglTerminate(dpy)
    # Row 0 read is the image's first row, its top: the texture's origin
    # is its memory's start. RGBA back into B, G, R, X, as rows() reads.
    src = bytes(rgba)
    px = bytearray(len(src))
    px[0::4], px[1::4], px[2::4] = src[2::4], src[1::4], src[0::4]
    return w, h, bytes(px)


def plane_props(fd, plane):
    """A plane's properties by name: the atomic ones, CRTC_X among them,
    which a client sees only with the atomic capability set."""
    return props_of(fd, plane, OBJECT_PLANE)


def props_of(fd, obj, kind):
    """An object's properties by name, with their values."""
    ids, vals = bytearray(4 * MAX_PROPS), bytearray(8 * MAX_PROPS)
    out = ioctl(fd, OBJ_GETPROPERTIES,
                struct.pack("QQIII4x", ctypes_address(ids),
                            ctypes_address(vals), MAX_PROPS, obj, kind))
    n = min(struct.unpack_from("I", out, 16)[0], MAX_PROPS)
    props = {}
    for i in range(n):
        pid = struct.unpack_from("I", ids, 4 * i)[0]
        got = ioctl(fd, GETPROPERTY, struct.pack("QQII32sII", 0, 0, pid, 0,
                                                 b"", 0, 0))
        name = got[24:56].split(b"\0", 1)[0].decode()
        props[name] = struct.unpack_from("q", vals, 8 * i)[0]
    return props


def crtc_active(fd, crtc):
    """The CRTC's atomic ACTIVE, which DPMS off clears and the legacy
    GETCRTC does not show; True where the kernel will not say."""
    try:
        ioctl(fd, SET_CLIENT_CAP, struct.pack("QQ", CAP_ATOMIC, 1))
        return props_of(fd, crtc, OBJECT_CRTC).get("ACTIVE", 1) != 0
    except OSError:
        return True


def cursor(fd, crtc):
    """The cursor plane on `crtc` -- (x, y, w, h, pitch, map) -- or None
    when it shows nothing. The KMS backend sets it by the legacy
    DRM_IOCTL_MODE_CURSOR, which the kernel turns into a framebuffer on
    the CRTC's cursor plane; that is what is read here, ARGB8888."""
    try:
        ioctl(fd, SET_CLIENT_CAP, struct.pack("QQ", CAP_ATOMIC, 1))
    except OSError:
        return None             # no atomic: the plane's place is not told
    ids = bytearray(4 * MAX_PLANES)
    out = ioctl(fd, GETPLANERESOURCES,
                struct.pack("QI4x", ctypes_address(ids), MAX_PLANES))
    n = min(struct.unpack_from("I", out, 8)[0], MAX_PLANES)
    for plane in struct.unpack_from("%dI" % n, ids):
        got = ioctl(fd, GETPLANE, struct.pack("6IQ", plane, 0, 0, 0, 0, 0, 0))
        _pid, on, fb_id = struct.unpack_from("3I", got)
        if on != crtc or not fb_id:
            continue
        props = plane_props(fd, plane)
        if props.get("type") != PLANE_TYPE_CURSOR:
            continue
        # One not ours to map is left out of the picture, which is still
        # the screen's; another cursor plane on the CRTC may yet be.
        try:
            fb = mapped(fd, fb_id)
        except OSError:
            continue
        if fb:
            return (props.get("CRTC_X", 0), props.get("CRTC_Y", 0)) + fb
    return None


def blend(rgb, w, h, cur):
    """The cursor's ARGB8888 over the picture's RGB rows, by its alpha:
    the plane's place may be off any edge, as at the hotspot's corner."""
    cx, cy, cw, ch, cpitch, cm = cur
    for j in range(ch):
        y = cy + j
        if not 0 <= y < h:
            continue
        row = cm[j * cpitch:j * cpitch + cw * 4]
        for i in range(cw):
            x = cx + i
            b, g, r, a = row[4 * i:4 * i + 4]
            if not a or not 0 <= x < w:
                continue
            o = (y * w + x) * 3
            for k, c in enumerate((r, g, b)):
                rgb[o + k] = (c * a + rgb[o + k] * (255 - a)) // 255


def modifier(fd, fb_id):
    """The framebuffer's format modifier, 0 being linear: the layout the
    rows are read in below. A driver's own scanout image -- the display
    path's, where the copy path's are dumb buffers -- may be tiled."""
    try:
        got = ioctl(fd, GETFB2, struct.pack("I", fb_id) + bytes(100))
    except OSError:
        return 0
    flags = struct.unpack_from("I", got, 16)[0]
    return struct.unpack_from("Q", got, 72)[0] if flags & FB_MODIFIERS else 0


def rows(m, w, h, pitch, mod, px=4):
    """Each scanline of the framebuffer, w * px bytes, read out of its
    layout: linear, or Intel's X or Y tiling, which anv gives its scanout
    images. A tile is 4 KiB: X's is 512 bytes by 8 rows, row after row; Y's
    is 128 bytes by 32 rows, as eight columns of 16 bytes, each column top
    to bottom. Tiles run left to right, pitch / tile width of them a row.
    Whole slices, not pixels, since this runs where there is no numpy."""
    if mod == MOD_LINEAR:
        for y in range(h):
            yield m[y * pitch:y * pitch + w * px]
    elif px != 4:
        sys.exit("a tiled framebuffer of %d bytes a pixel is a layout this "
                 "cannot read" % px)
    elif mod == MOD_INTEL_X:
        across = pitch // 512
        for y in range(h):
            base = (y // 8) * across * 4096 + (y % 8) * 512
            yield b"".join(m[base + t * 4096:base + t * 4096 + 512]
                           for t in range(across))[:w * 4]
    elif mod == MOD_INTEL_Y:
        across = pitch // 128
        for y in range(h):
            base = (y // 32) * across * 4096 + (y % 32) * 16
            yield b"".join(m[base + t * 4096 + c * 512:
                             base + t * 4096 + c * 512 + 16]
                           for t in range(across) for c in range(8))[:w * 4]
    else:
        sys.exit("framebuffer modifier %#x is a layout this cannot read"
                 % mod)


def main():
    # `--output NAME`, then the card named, or each in turn: a machine with
    # two GPUs has its panel on whichever the kernel numbered second as
    # often as not.
    args = sys.argv[1:]
    output = None
    if args[:1] == ["--output"] and len(args) >= 2:
        output, args = args[1], args[2:]
    cards = args or sorted(glob.glob("/dev/dri/card[0-9]*"))
    # Two cards can each have a connector of that name, and the one that
    # shows something can be the later: why the others did not, for when
    # none does.
    why = None
    for card in cards:
        # A card that will not open is not this one: a GPU with no outputs
        # on nouveau answers ENODEV, and the panel's card comes after it.
        try:
            fd = os.open(card, os.O_RDWR | os.O_CLOEXEC)
        except OSError:
            continue
        try:
            if not modesetting(fd):
                continue
            if output is None:
                if grab(fd, card, crtcs(fd)):
                    return 0
                continue
            crtc = connector_crtc(fd, output)
            if crtc is None:
                continue
            if not crtc:
                why = "%s on %s is driven by no CRTC" % (output, card)
                continue
            if grab(fd, card, (crtc,)):
                return 0
            why = ("%s on %s shows nothing: its CRTC %d is off or has no "
                   "framebuffer" % (output, card, crtc))
        finally:
            os.close(fd)
    if output is not None:
        sys.exit(why or "no card has a connector %s" % output)
    sys.exit("no CRTC is scanning anything out on %s" % ", ".join(cards))


def grab(fd, card, among):
    """The first CRTC of `among` that is lit with a framebuffer on it,
    written out as a PPM; False when none is. A CRTC that is off -- a
    connector's DPMS off, as the KMS backend leaves an output it left --
    still names the framebuffer it last showed, and is passed over."""
    for crtc in among:
        got = ioctl(fd, GETCRTC, struct.pack("QIII", 0, 0, crtc, 0)
                    + bytes(104 - 20))
        fb_id, = struct.unpack_from("I", got, 16)
        mode_valid, = struct.unpack_from("I", got, 32)
        if not fb_id or not mode_valid or not crtc_active(fd, crtc):
            continue
        mod = modifier(fd, fb_id)
        px = pixel_bytes(fd, fb_id)
        # Not ours to map -- another client's buffer (EACCES), or one the
        # driver made itself, i915's console framebuffer, which is no dumb
        # buffer (ENODEV) -- and read back by the GPU instead.
        try:
            fb = mapped(fd, fb_id, TILE_ROWS.get(mod, 1))
        except (PermissionError, OSError):
            how = "read back by the GPU"
            w, h, snap = gpu_read(fd, fb_id)
            pitch, mod, px = w * 4, MOD_LINEAR, 4
        else:
            if not fb:
                sys.exit("no handle for the framebuffer: run this as root")
            how = "mapped"
            w, h, pitch, m = fb
            # Copied out whole before it is decoded: 30 ms or so where
            # decoding out of the mapping takes seconds, through which the
            # program draws into this image again each time the swapchain
            # comes round to it, and the picture would be rows of frames a
            # cycle apart.
            snap = m[:]
            m.close()
        # The pointer as it was with that frame, not seconds later.
        cur = cursor(fd, crtc)
        rgb = bytearray(w * h * 3)
        for y, row in enumerate(rows(snap, w, h, pitch, mod, px)):
            o = y * w * 3
            if px == 2:
                # RGB565, little-endian: each channel widened to 8 bits.
                v = struct.unpack("<%dH" % w, row)
                rgb[o:o + w * 3:3] = bytes((p >> 11) * 255 // 31 for p in v)
                rgb[o + 1:o + w * 3:3] = bytes((p >> 5 & 63) * 255 // 63
                                               for p in v)
                rgb[o + 2:o + w * 3:3] = bytes((p & 31) * 255 // 31
                                               for p in v)
                continue
            # XRGB8888 is B, G, R, X in memory; a PPM is R, G, B.
            rgb[o:o + w * 3:3] = row[2::4]
            rgb[o + 1:o + w * 3:3] = row[1::4]
            rgb[o + 2:o + w * 3:3] = row[0::4]
        if cur:
            blend(rgb, w, h, cur)
            cur[5].close()
        out = sys.stdout.buffer
        out.write(b"P6\n%d %d\n255\n" % (w, h))
        out.write(rgb)
        print("%s, crtc %d, framebuffer %d, %dx%d, modifier %#x, %s; %s"
              % (card, crtc, fb_id, w, h, modifier(fd, fb_id), how,
                 "cursor %dx%d at %d,%d" % (cur[2], cur[3], cur[0], cur[1])
                 if cur else "no cursor"), file=sys.stderr)
        return True
    return False


if __name__ == "__main__":
    sys.exit(main())
