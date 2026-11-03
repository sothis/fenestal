/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The KMS-to-EGL bridge's half, and the smallest of them: there is no window,
 * and nothing here touches the DRM node. The display is Mesa's surfaceless
 * one (EGL_MESA_platform_surfaceless), which renders on the GPU's render node
 * -- or on llvmpipe under LIBGL_ALWAYS_SOFTWARE -- with the context current
 * on no surface at all. The frame is drawn into the renderer's own window
 * image and blitted on the GPU into the backend's scanout buffer -- the
 * backend's pair of dumb buffers, exported as dma-bufs and imported here as
 * EGL images, each once -- which the backend then flips on, owning the
 * output, its mode and its buffers as before (kms_scan_begin(),
 * kms_scan_show()). No libgbm: a dumb buffer is linear XRGB, which every
 * driver that imports a dma-buf takes, and the backend links nothing.
 *
 * Where any of it is refused -- the kernel will not export the buffer, the
 * display has no EGL_EXT_image_dma_buf_import, the GPU drawing is not the
 * one the buffer is on and will not take it, GL has no GL_OES_EGL_image --
 * the frame is read back and handed to the backend instead
 * (FnlBackendVtbl::present_pixels), the path Vulkan's copy path takes where
 * no device takes the display (DESIGN *KMS: no window server*). Decided at
 * the first frame, said once, and kept.
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"
#include "renderer/gl/gl_egl.h"
#include "backend/kms/kms_state.h"

#include <unistd.h>

/* GL_OES_EGL_image's way from an EGL image to a renderbuffer, which
 * desktop GL's headers do not carry: asked for by name, so a driver without
 * it keeps the copy path. A texture over the image is not tried: NVIDIA's
 * EGL makes one over the Intel GPU's buffer and draws into a copy of its
 * own, and destroying that image then crashed it inside
 * eglDestroyImageKHR. */
typedef void (*ImageToTarget)(GLenum target, void *image);

/* How long the blit into a scanout buffer is waited for before the flip,
 * at most: the kernel flips a dumb buffer whatever the GPU is still doing
 * to it where the driver does not wait on its fence. */
#define SCAN_WAIT_NS 100000000ull   /* 100 ms */

static bool native_display(App *a, EglNative *out)
{
	(void)a;
	*out = (EglNative){
		.platform = EGL_PLATFORM_SURFACELESS_MESA,
		.display  = EGL_DEFAULT_DISPLAY,
		.attrs    = { EGL_NONE },
	};
	return true;
}

static EGLint native_visual(const App *a)
{
	(void)a;
	return 0;
}

/* The output's plane is opaque XRGB, and the pixels go to it by copy. */
static EGLint native_alpha(const App *a)
{
	(void)a;
	return 0;
}

/* The backend's flip is the throttle: present_pixels() waits for the last
 * one under vsync, and without it flips at once, tearing (kms.c). A
 * driver that refuses the asynchronous flip says so in the log; this line
 * does not know. */
static const char *native_pacer(const App *a)
{
	return fnl_atomic_load_u32(&a->present_mode) == 1u
			   ? "page flip (vsync)" : "page flip (async)";
}

/* Never asked for: a surfaceless bridge makes no window surface (egl.c). */
static void *native_window(App *a)
{
	(void)a;
	return NULL;
}

static void native_window_resize(App *a, int w, int h)
{
	(void)a;
	(void)w;
	(void)h;
}

static void native_window_close(App *a)
{
	(void)a;
}

/* Give the way up for good, with the reason, once. */
static bool scan_give_up(App *a, const char *why)
{
	if (!GLS(a)->scan_off)
		fnl_log(FNL_LOG_INFO, "KMS: OpenGL copies each frame onto the "
				"output: %s", why);
	GLS(a)->scan_off = true;
	return false;
}

/* The display's and not the context's, so no GL call. */
static void scan_image_drop(App *a, int i)
{
	if (GLS(a)->scan_img[i]) {
		PFNEGLDESTROYIMAGEKHRPROC destroy = (PFNEGLDESTROYIMAGEKHRPROC)
			eglGetProcAddress("eglDestroyImageKHR");
		if (destroy)
			destroy(GLS(a)->bridge_display, GLS(a)->scan_img[i]);
	}
	GLS(a)->scan_img[i] = NULL;
}

static void scan_drop(App *a, int i)
{
	if (GLS(a)->scan_fbo[i])
		gl.DeleteFramebuffers(1, &GLS(a)->scan_fbo[i]);
	if (GLS(a)->scan_rb[i])
		gl.DeleteRenderbuffers(1, &GLS(a)->scan_rb[i]);
	scan_image_drop(a, i);
	GLS(a)->scan_fbo[i] = GLS(a)->scan_rb[i] = 0;
	GLS(a)->scan_key[i] = 0;
}

/* The framebuffer over what `i`'s image was attached to, complete; or
 * false, with what went wrong in `why` and the framebuffer gone. */
static bool scan_fbo_check(App *a, int i, const char *how, char *why,
						   size_t size)
{
	const GLenum err = gl.GetError();
	const GLenum st = err == GL_NO_ERROR
		? gl.CheckNamedFramebufferStatus(GLS(a)->scan_fbo[i],
										 GL_DRAW_FRAMEBUFFER)
		: 0;
	if (err == GL_NO_ERROR && st == GL_FRAMEBUFFER_COMPLETE)
		return true;
	if (err != GL_NO_ERROR)
		snprintf(why, size, "%s: GL error 0x%04x", how, (unsigned)err);
	else
		snprintf(why, size, "%s: framebuffer status 0x%04x", how,
				 (unsigned)st);
	gl.DeleteFramebuffers(1, &GLS(a)->scan_fbo[i]);
	GLS(a)->scan_fbo[i] = 0;
	return false;
}

/* `i`'s image behind a renderbuffer (GL_OES_EGL_image): what Mesa takes. */
static bool scan_attach_rb(App *a, int i, void *img, char *why, size_t size)
{
	const ImageToTarget to_rb = (ImageToTarget)
		gl_bridge_proc("glEGLImageTargetRenderbufferStorageOES");
	if (!to_rb) {
		snprintf(why, size, "a renderbuffer: no GL_OES_EGL_image");
		return false;
	}
	gl_clear_errors();
	gl.CreateRenderbuffers(1, &GLS(a)->scan_rb[i]);
	gl.BindRenderbuffer(GL_RENDERBUFFER, GLS(a)->scan_rb[i]);
	/* Asked, not assumed: NVIDIA's GL refuses the image here
	 * (GL_INVALID_OPERATION, "EGLImage not supported") and the frames go
	 * the copy path's way, so its error is not the debug output's to log. */
	GLS(a)->debug_quiet = true;
	to_rb(GL_RENDERBUFFER, img);
	GLS(a)->debug_quiet = false;
	gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
	gl.CreateFramebuffers(1, &GLS(a)->scan_fbo[i]);
	gl.NamedFramebufferRenderbuffer(GLS(a)->scan_fbo[i], GL_COLOR_ATTACHMENT0,
									GL_RENDERBUFFER, GLS(a)->scan_rb[i]);
	if (scan_fbo_check(a, i, "a renderbuffer", why, size))
		return true;
	gl.DeleteRenderbuffers(1, &GLS(a)->scan_rb[i]);
	GLS(a)->scan_rb[i] = 0;
	return false;
}

/* Whether what GL draws into `i`'s framebuffer lands in the backend's
 * buffer: a colour cleared on the GPU, waited for, and read back through
 * the backend's mapping at both corners. NVIDIA's EGL, given a texture
 * over the Intel GPU's buffer, made a complete framebuffer of it and drew
 * into a copy of its own -- the panel stayed black, and nothing said so;
 * a driver may do as much behind a renderbuffer. A colour no frame starts
 * with, so a buffer that merely held it already does not pass. */
static bool scan_lands(App *a, int i)
{
	static const float PROBE[4] = { 0x12 / 255.0f, 0x34 / 255.0f,
									0x56 / 255.0f, 1.0f };
	/* A clear honours the write mask, which a blit does not, and the
	 * scissor, which the frame has turned off by now. */
	gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	gl.ClearNamedFramebufferfv(GLS(a)->scan_fbo[i], GL_COLOR, 0, PROBE);
	gl.Finish();
	uint32_t first = 0, last = 0;
	if (!kms_scan_peek(a, i, &first, &last))
		return false;
	/* XRGB8888 in memory, read as a little-endian word: 0x00RRGGBB. */
	return (first & 0xffffffu) == 0x123456u &&
		   (last & 0xffffffu) == 0x123456u;
}

/* Buffer `i` of the backend's pair as a framebuffer to blit into: the one
 * made for it, or one made now from its dma-buf where the pair is new.
 * False where it will not be had, the way given up. */
static bool scan_import(App *a, int i)
{
	if (GLS(a)->scan_fbo[i] && GLS(a)->scan_key[i] == kms_scan_key(a, i))
		return true;
	scan_drop(a, i);
	const char *exts = eglQueryString(GLS(a)->bridge_display,
									  EGL_EXTENSIONS);
	if (!exts || !strstr(exts, "EGL_EXT_image_dma_buf_import"))
		return scan_give_up(a, "the EGL display has no "
							   "EGL_EXT_image_dma_buf_import");
	PFNEGLCREATEIMAGEKHRPROC create = (PFNEGLCREATEIMAGEKHRPROC)
		eglGetProcAddress("eglCreateImageKHR");
	if (!create)
		return scan_give_up(a, "no eglCreateImageKHR");
	KmsScanBuf b;
	const int fd = kms_scan_export(a, i, &b);
	if (fd < 0)
		return scan_give_up(a, "the kernel will not export the scanout "
							   "buffer");
	/* Linear, said where the display takes a modifier: a driver that
	 * reads none from the buffer would otherwise guess its own tiling. */
	const bool mods = strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers");
	EGLint attrs[20];
	int n = 0;
	attrs[n++] = EGL_WIDTH;                     attrs[n++] = b.w;
	attrs[n++] = EGL_HEIGHT;                    attrs[n++] = b.h;
	attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;      attrs[n++] = (EGLint)b.fourcc;
	attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attrs[n++] = fd;
	attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[n++] = 0;
	attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attrs[n++] = (EGLint)b.pitch;
	if (mods) {
		attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attrs[n++] = 0;
		attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attrs[n++] = 0;
	}
	attrs[n] = EGL_NONE;
	EGLImageKHR img = create(GLS(a)->bridge_display, EGL_NO_CONTEXT,
							 EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	/* The image holds the buffer; the descriptor was only the way in. */
	close(fd);
	if (img == EGL_NO_IMAGE_KHR)
		return scan_give_up(a, "the GPU drawing will not import the "
							   "output's buffer");
	GLS(a)->scan_img[i] = img;
	char rb_why[96];
	if (!scan_attach_rb(a, i, img, rb_why, sizeof rb_why)) {
		scan_drop(a, i);
		char why[160];
		snprintf(why, sizeof why, "the imported buffer is no framebuffer "
				 "(%s)", rb_why);
		return scan_give_up(a, why);
	}
	if (!scan_lands(a, i)) {
		scan_drop(a, i);
		return scan_give_up(a, "the GPU's writes do not reach the "
							   "output's buffer");
	}
	GLS(a)->scan_key[i] = kms_scan_key(a, i);
	return true;
}

/* The window's image into the back buffer of the backend's pair, on the
 * GPU: the same rows, row 0 the top in both, the bytes as they are -- the
 * image is sRGB-encoded already, and the buffer is plain XRGB. */
static bool kms_scan_begin_gl(App *a, unsigned fbo, int w, int h)
{
	GLS(a)->scan_direct = false;
	GLS(a)->scan_back = -1;
	if (GLS(a)->scan_off)
		return false;
	if (FNL_FAULT("kms-gl-copy"))
		return scan_give_up(a, "FNL_FAULT");
	const int back = kms_scan_begin(a);
	GLS(a)->scan_direct = true;
	if (back < 0)
		return true;
	if (!scan_import(a, back)) {
		GLS(a)->scan_direct = false;
		return false;
	}
	if (!GLS(a)->scan_said) {
		fnl_log(FNL_LOG_INFO, "KMS: OpenGL draws onto the output's own "
				"buffers, nothing read back");
		GLS(a)->scan_said = true;
	}
	/* The window is the output, so the two are one size; a blit past the
	 * buffer's edge is clipped to it, as the copy path's is. */
	gl.BlitNamedFramebuffer(fbo, GLS(a)->scan_fbo[back], 0, 0, w, h,
							0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	GLS(a)->scan_back = back;
	return true;
}

/* The blit waited for, then the flip: the kernel shows a buffer as it is
 * when it scans it out, and a driver need not wait on the GPU's writes to
 * a dumb buffer first. A wait that blocks, so the CPU half leaves it out. */
static bool kms_scan_show_gl(App *a)
{
	const int back = GLS(a)->scan_back;
	if (back < 0)
		return false;
	GLsync s = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (s) {
		(void)gl.ClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT, SCAN_WAIT_NS);
		gl.DeleteSync(s);
	}
	kms_scan_show(a, back);
	return true;
}

/* With the device: another GPU may take the buffers this one refused. */
static void kms_scan_close_gl(App *a)
{
	for (int i = 0; i < 2; i++)
		scan_drop(a, i);
	GLS(a)->scan_off = GLS(a)->scan_said = false;
	GLS(a)->scan_direct = false;
	GLS(a)->scan_back = -1;
}

/* With the context gone unmade: its renderbuffers went with it, and the
 * images, the display's, which outlives it, are destroyed here. The rest
 * goes with the state (device.c's forget_objects()). */
static void kms_scan_forget_gl(App *a)
{
	for (int i = 0; i < 2; i++)
		scan_image_drop(a, i);
}

const EglBridge egl_bridge_kms = {
	.backend       = BACKEND_KMS,
	.surfaceless   = true,
	.display       = native_display,
	.visual        = native_visual,
	.alpha         = native_alpha,
	.pacer         = native_pacer,
	.window        = native_window,
	.window_close  = native_window_close,
	.window_resize = native_window_resize,
	.scan_begin    = kms_scan_begin_gl,
	.scan_show     = kms_scan_show_gl,
	.scan_close    = kms_scan_close_gl,
	.scan_forget   = kms_scan_forget_gl,
};
