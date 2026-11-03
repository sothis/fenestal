/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The OpenGL renderer's bridge on macOS: a CGL context with no view, and
 * its frame handed to the window's CAMetalLayer by Metal -- the one way out
 * a Mac's window has for frames that are not Metal's own (DESIGN *OpenGL on
 * macOS*). egl.c's sibling, implementing the same gl_bridge_* entries, and
 * the one file of the GL renderer that sees the window and Metal both.
 *
 * The context is 4.1 core, the most macOS gives, so the renderer draws
 * through its 4.1 profile (gl_compat.c) and a scene of the caller's is told
 * so (fnl_gl_version()). There is no window surface: the bridge says it is
 * surfaceless, and the frame takes the way KMS's own buffers take
 * (gl_bridge_scan_begin(), _show()): the window's image is blitted into an
 * IOSurface, one for each frame slot, bound as a rectangle texture
 * (CGLTexImageIOSurface2D) -- unflipped, since an IOSurface's row 0 is its
 * top and GL takes it for the bottom, which is the window image's own
 * convention -- and Metal copies the IOSurface, wrapped as a BGRA8 sRGB
 * texture so the copy moves the encoded bytes as they are, into the
 * layer's drawable and presents it. nextDrawable is the throttle, and the
 * layer's displaySyncEnabled the interval.
 *
 * GL's writes are waited for before Metal reads (a fence, in the present):
 * whether glFlush alone orders them is unmeasured, and the wait costs the
 * overlap of the two only. Rejected: NSOpenGLContext on the view, whose
 * `update` is the main thread's at every size and races the render
 * thread; CAOpenGLLayer, which draws on Core Animation's thread; an
 * IOSurface as a plain layer's `contents`, undocumented, with no throttle;
 * ANGLE or Zink, a library to ship.
 */

#define GL_SILENCE_DEPRECATION

#include "core/app.h"

#include "renderer/gl/gl_internal.h"

#include "backend/cocoa/cocoa_state.h"

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLIOSurface.h>

#include <dlfcn.h>

/* How long the present waits for GL's writes before giving the frame up,
 * and how long it sleeps between two looks at GL's fence. */
#define CGL_WAIT_S 0.1
#define CGL_POLL_S 0.0002

/* GL's rectangle target and the IOSurface's client format, as glcorearb.h
 * spells them. */
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#  define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif

/* What the bridge keeps beside the context: Metal's device and queue for
 * the copy, the window's layer, and per frame slot the IOSurface, GL's
 * texture and framebuffer over it, Metal's texture over it and the command
 * buffer that last read it. Kept in RendererState::bridge_native. */
@interface FnlCglBridge : NSObject {
@public
	id<MTLDevice>        device;
	id<MTLCommandQueue>  queue;
	CAMetalLayer        *layer;
	IOSurfaceRef         surf[MAX_FRAMES_IN_FLIGHT];
	GLuint               tex[MAX_FRAMES_IN_FLIGHT];
	GLuint               fbo[MAX_FRAMES_IN_FLIGHT];
	id<MTLTexture>       mtex[MAX_FRAMES_IN_FLIGHT];
	id<MTLCommandBuffer> cb[MAX_FRAMES_IN_FLIGHT];
	int                  w[MAX_FRAMES_IN_FLIGHT], h[MAX_FRAMES_IN_FLIGHT];
	int                  slot;      /* this frame's, from scan_begin */
	int                  next;
	GLsync               written;   /* GL's blit into the slot's surface */
	bool                 said;
}
@end

@implementation FnlCglBridge
@end

static FnlCglBridge *bridge(const App *a)
{
	return (__bridge FnlCglBridge *)GLS(a)->bridge_native;
}

/* ---- the library and the instance ---------------------------------------- */

/* OpenGL's framework, for the entry points: linked, and asked by name, as
 * every GL entry is asked of a bridge. */
static void *gl_lib(void)
{
	static void *lib;
	if (!lib)
		lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL",
					 RTLD_LAZY | RTLD_LOCAL);
	return lib;
}

FnlGlProc gl_bridge_proc(const char *name)
{
	void *lib = gl_lib();
	return lib ? (FnlGlProc)dlsym(lib, name) : NULL;
}

FnlResult gl_bridge_hold(App *a)
{
	(void)a;
	if (!gl_lib())
		return fnl_fail(FNL_ERR_NO_DEVICE, "no OpenGL framework: %s",
						dlerror());
	return FNL_OK;
}

void gl_bridge_let_go(App *a)
{
	(void)a;
}

/* The framework is never closed, so a hold on it open is any hold. */
bool gl_bridge_hold_open(App *a)
{
	return gl_bridge_hold(a) == FNL_OK;
}

/* 4.1 core, which is what "GL4_Core" gives on every Mac that has OpenGL;
 * accelerated, and on a machine with two GPUs the one not driving the
 * display allowed too. No double buffer: there is no drawable of GL's. */
static CGLPixelFormatObj pixel_format(void)
{
	const CGLPixelFormatAttribute attrs[] = {
		kCGLPFAOpenGLProfile,
		(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
		kCGLPFAAccelerated,
		kCGLPFAAllowOfflineRenderers,
		kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
		kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
		(CGLPixelFormatAttribute)0,
	};
	CGLPixelFormatObj pf = NULL;
	GLint n = 0;
	if (CGLChoosePixelFormat(attrs, &pf, &n) != kCGLNoError || !pf)
		return NULL;
	return pf;
}

FnlResult gl_bridge_open(App *a)
{
	if (!a->backend_state || !a->backend_state->layer)
		return fnl_fail(FNL_ERR_NO_DISPLAY, "no layer to draw into");
	if (GLS(a)->bridge_display)
		return FNL_OK;
	CGLPixelFormatObj pf = pixel_format();
	if (!pf)
		return fnl_fail(FNL_ERR_NO_DEVICE, "no OpenGL 4.1 core pixel "
						"format on this Mac");
	GLS(a)->bridge_display = pf;
	FnlCglBridge *b = [FnlCglBridge new];
	b->layer = (__bridge CAMetalLayer *)a->backend_state->layer;
	GLS(a)->bridge_native = (__bridge_retained void *)b;
	return FNL_OK;
}

void gl_bridge_close(App *a)
{
	if (GLS(a)->bridge_native) {
		FnlCglBridge *b = (__bridge_transfer FnlCglBridge *)
			GLS(a)->bridge_native;
		(void)b;
		GLS(a)->bridge_native = NULL;
	}
	if (GLS(a)->bridge_display)
		CGLReleasePixelFormat(GLS(a)->bridge_display);
	GLS(a)->bridge_display = NULL;
}

/* No surface: the layer is the window's, made with its view. */
FnlResult gl_bridge_surface(App *a)
{
	(void)a;
	return FNL_OK;
}

void gl_bridge_surface_close(App *a)
{
	(void)a;
}

bool gl_bridge_surfaceless(const App *a)
{
	(void)a;
	return true;
}

/* ---- the context --------------------------------------------------------- */

FnlResult gl_bridge_context(App *a)
{
	CGLContextObj ctx = NULL;
	const CGLError e = CGLCreateContext(GLS(a)->bridge_display, NULL, &ctx);
	if (e != kCGLNoError || !ctx)
		return fnl_fail(FNL_ERR_NO_DEVICE, "CGLCreateContext failed: %s",
						CGLErrorString(e));
	GLS(a)->bridge_context = ctx;
	/* CGL has no reset notification: a lost device is not noticed, as on
	 * any context that is not robust. */
	GLS(a)->robust = false;
	if (CGLSetCurrentContext(ctx) != kCGLNoError)
		return fnl_fail(FNL_ERR_SYSTEM, "CGLSetCurrentContext failed");
	return FNL_OK;
}

void gl_bridge_context_close(App *a)
{
	if (!GLS(a)->bridge_context)
		return;
	if (CGLGetCurrentContext() == GLS(a)->bridge_context)
		CGLSetCurrentContext(NULL);
	CGLReleaseContext(GLS(a)->bridge_context);
	GLS(a)->bridge_context = NULL;
}

bool gl_bridge_current(App *a, bool on)
{
	if (!GLS(a)->bridge_context)
		return true;
	const CGLError e = CGLSetCurrentContext(on ? GLS(a)->bridge_context
											   : NULL);
	if (e != kCGLNoError && on) {
		fnl_log(FNL_LOG_ERROR, "CGLSetCurrentContext failed: %s",
				CGLErrorString(e));
		return false;
	}
	return true;
}

/* ---- the layer ----------------------------------------------------------- */

/* The layer's drawables at the window image's size, and its interval: the
 * drawable size set on the render thread inside a transaction of its own
 * with the implicit animation off, as the Metal renderer sets it. The
 * layer is handed this bridge's device and format again at every build:
 * another renderer may have drawn into it before. */
FnlResult gl_bridge_resize(App *a, int w, int h, bool opaque)
{
	(void)opaque;
	FnlCglBridge *b = bridge(a);
	if (!b)
		return FNL_OK;
	const bool vsync = fnl_atomic_load_bool(&a->vsync);
	/* The render thread has no pool outside a frame, and the transaction
	 * autoreleases, as Metal's swapchain rebuild finds. */
	@autoreleasepool {
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		if (b->device) {
			b->layer.device = b->device;
			b->layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
			/* Copied into, which a framebuffer-only drawable refuses. */
			b->layer.framebufferOnly = NO;
		}
		b->layer.drawableSize = CGSizeMake(w, h);
		b->layer.displaySyncEnabled = vsync;
		[CATransaction commit];
	}
	return FNL_OK;
}

/* The interval is the layer's (gl_bridge_resize()); nothing to give. */
bool gl_bridge_interval(App *a, int n)
{
	(void)a; (void)n;
	return true;
}

/* Any thread's (fnl_present_mode()), so the atomic and not the bridge,
 * which a move to another renderer frees: gl_window_image() mirrors the
 * same vsync it hands the layer above. */
const char *gl_bridge_pacer(const App *a)
{
	return fnl_atomic_load_u32(&a->present_mode) == 1u
			   ? "Metal layer (vsync)" : "Metal layer";
}

bool gl_bridge_occluded(const App *a)
{
	return a->backend_state &&
		   fnl_atomic_load_bool(&a->backend_state->occluded);
}

/* Never asked: a surfaceless bridge presents through scan_show. */
GlSwap gl_bridge_swap(App *a)
{
	(void)a;
	return GL_SWAP_FAILED;
}

/* ---- the frame's way out ------------------------------------------------- */

/* The Metal device that is the GL context's renderer, by its registry id;
 * the layer's preferred one, else the system's, where CGL cannot say. */
static id<MTLDevice> device_of(CGLContextObj ctx, CAMetalLayer *layer)
{
	GLint renderer = 0;
	CGLRendererInfoObj info = NULL;
	GLint n = 0;
	uint64_t want = 0;
	if (CGLGetParameter(ctx, kCGLCPCurrentRendererID, &renderer) ==
			kCGLNoError &&
		CGLQueryRendererInfo(0xffffffffu, &info, &n) == kCGLNoError) {
		for (GLint i = 0; i < n && !want; i++) {
			GLint id = 0, lo = 0, hi = 0;
			if (CGLDescribeRenderer(info, i, kCGLRPRendererID, &id) !=
					kCGLNoError ||
				(id & kCGLRendererIDMatchingMask) !=
					(renderer & kCGLRendererIDMatchingMask))
				continue;
			if (CGLDescribeRenderer(info, i, kCGLRPRegistryIDLow, &lo) ==
					kCGLNoError &&
				CGLDescribeRenderer(info, i, kCGLRPRegistryIDHigh, &hi) ==
					kCGLNoError)
				want = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
		}
		CGLDestroyRendererInfo(info);
	}
	if (want)
		for (id<MTLDevice> d in MTLCopyAllDevices())
			if (d.registryID == want)
				return d;
	return layer.preferredDevice ?: MTLCreateSystemDefaultDevice();
}

/* The slot let go, GL's names with it: the context is current. */
static void slot_drop(FnlCglBridge *b, int i)
{
	if (b->fbo[i])
		gl.DeleteFramebuffers(1, &b->fbo[i]);
	if (b->tex[i])
		gl.DeleteTextures(1, &b->tex[i]);
	b->fbo[i] = b->tex[i] = 0;
	b->mtex[i] = nil;
	if (b->surf[i])
		CFRelease(b->surf[i]);
	b->surf[i] = NULL;
	b->w[i] = b->h[i] = 0;
}

/* The slot's IOSurface at w x h, with GL's texture and framebuffer and
 * Metal's texture over it, made again at another size. False where any
 * part was refused, with the slot let go. */
static bool slot_make(App *a, FnlCglBridge *b, int i, int w, int h)
{
	if (b->surf[i] && b->w[i] == w && b->h[i] == h)
		return true;
	slot_drop(b, i);
	NSDictionary *props = @{
		(id)kIOSurfaceWidth: @(w),
		(id)kIOSurfaceHeight: @(h),
		(id)kIOSurfaceBytesPerElement: @4,
		(id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
	};
	b->surf[i] = IOSurfaceCreate((__bridge CFDictionaryRef)props);
	if (!b->surf[i])
		return false;
	gl_clear_errors();
	gl.GenTextures(1, &b->tex[i]);
	gl.BindTexture(GL_TEXTURE_RECTANGLE, b->tex[i]);
	const CGLError e = CGLTexImageIOSurface2D(
		GLS(a)->bridge_context, GL_TEXTURE_RECTANGLE, GL_RGBA, (GLsizei)w,
		(GLsizei)h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, b->surf[i], 0);
	gl.BindTexture(GL_TEXTURE_RECTANGLE, 0);
	if (e != kCGLNoError) {
		fnl_log(FNL_LOG_ERROR, "CGLTexImageIOSurface2D failed: %s",
				CGLErrorString(e));
		slot_drop(b, i);
		return false;
	}
	gl.GenFramebuffers(1, &b->fbo[i]);
	GLint was = 0;
	gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &was);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo[i]);
	gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
							GL_TEXTURE_RECTANGLE, b->tex[i], 0);
	const GLenum st = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)was);
	if (st != GL_FRAMEBUFFER_COMPLETE || gl.GetError() != GL_NO_ERROR) {
		fnl_log(FNL_LOG_ERROR, "the IOSurface's framebuffer is not "
				"complete (0x%04x)", (unsigned)st);
		slot_drop(b, i);
		return false;
	}
	MTLTextureDescriptor *d = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
									 width:(NSUInteger)w
									height:(NSUInteger)h
								 mipmapped:NO];
	d.usage = MTLTextureUsageShaderRead;
	d.storageMode = MTLStorageModeManaged;
	b->mtex[i] = [b->device newTextureWithDescriptor:d
										   iosurface:b->surf[i]
											   plane:0];
	if (!b->mtex[i]) {
		fnl_log(FNL_LOG_ERROR, "Metal would not wrap the IOSurface");
		slot_drop(b, i);
		return false;
	}
	b->w[i] = w;
	b->h[i] = h;
	return true;
}

/* The window's image into this slot's IOSurface, on the GPU, the bytes as
 * they are. A slot is written only once Metal is done reading it. */
bool gl_bridge_scan_begin(App *a, unsigned fbo, int w, int h)
{
	FnlCglBridge *b = bridge(a);
	GLS(a)->scan_direct = false;
	if (!b || w <= 0 || h <= 0)
		return false;
	@autoreleasepool {
		if (!b->device) {
			b->device = device_of(GLS(a)->bridge_context, b->layer);
			b->queue = [b->device newCommandQueue];
			if (!b->device || !b->queue) {
				b->device = nil;
				return false;
			}
			/* The layer is told its device and format now that there is
			 * one; its size and interval came with the window image. */
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			b->layer.device = b->device;
			b->layer.pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
			b->layer.framebufferOnly = NO;
			[CATransaction commit];
		}
		const int i = b->next;
		if (b->cb[i]) {
			[b->cb[i] waitUntilCompleted];
			b->cb[i] = nil;
		}
		if (!slot_make(a, b, i, w, h))
			return false;
		if (!b->said) {
			fnl_log(FNL_LOG_INFO, "OpenGL draws into an IOSurface, which "
					"Metal copies onto %s", b->device.name.UTF8String);
			b->said = true;
		}
		gl.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo[i]);
		gl.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT,
						   GL_NEAREST);
		if (b->written)
			gl.DeleteSync(b->written);
		b->written = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		gl.Flush();
		b->slot = i;
		b->next = (i + 1) % MAX_FRAMES_IN_FLIGHT;
	}
	GLS(a)->scan_direct = true;
	return true;
}

/* GL's writes waited for, then the copy into the layer's drawable and its
 * present. A drawable that does not come -- the layer's second spent, a
 * window nobody is shown -- is a frame not shown.
 *
 * The fence is looked at and slept between, not waited on: Apple's
 * glClientWaitSync spins, 4.4 to 4.8 ms of CPU a frame on schenker's HD 630
 * with the scene moving, for a wait the polled one makes as long (4.3 to
 * 5.0 ms) in 0.24 to 0.29 ms of CPU. Not dropped: with no wait the same
 * time went into nextDrawable, asleep, but nothing then orders GL's writes
 * before Metal's read of the IOSurface, two queues of one GPU. */
bool gl_bridge_scan_show(App *a)
{
	FnlCglBridge *b = bridge(a);
	if (!b || !b->device)
		return false;
	if (b->written) {
		const double until = now_seconds() + CGL_WAIT_S;
		GLbitfield flags = GL_SYNC_FLUSH_COMMANDS_BIT;
		for (;;) {
			const GLenum r = gl.ClientWaitSync(b->written, flags, 0);
			if (r != GL_TIMEOUT_EXPIRED || now_seconds() > until)
				break;
			flags = 0;
			fnl_sleep_seconds(CGL_POLL_S);
		}
		gl.DeleteSync(b->written);
		b->written = NULL;
	}
	@autoreleasepool {
		const int i = b->slot;
		id<CAMetalDrawable> drawable = [b->layer nextDrawable];
		if (!drawable || !b->mtex[i])
			return false;
		id<MTLTexture> dst = drawable.texture;
		const NSUInteger w = MIN(dst.width, (NSUInteger)b->w[i]);
		const NSUInteger h = MIN(dst.height, (NSUInteger)b->h[i]);
		id<MTLCommandBuffer> cb = [b->queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:b->mtex[i]
				  sourceSlice:0
				  sourceLevel:0
				 sourceOrigin:MTLOriginMake(0, 0, 0)
				   sourceSize:MTLSizeMake(w, h, 1)
					toTexture:dst
			 destinationSlice:0
			 destinationLevel:0
			destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		[cb presentDrawable:drawable];
		[cb commit];
		b->cb[i] = cb;
	}
	return true;
}

/* With the device: the slots are GL names of this context and Metal's
 * textures over surfaces made for it. */
void gl_bridge_scan_close(App *a)
{
	FnlCglBridge *b = bridge(a);
	if (!b)
		return;
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (b->cb[i])
			[b->cb[i] waitUntilCompleted];
		b->cb[i] = nil;
		slot_drop(b, i);
	}
	if (b->written)
		gl.DeleteSync(b->written);
	b->written = NULL;
	b->queue = nil;
	b->device = nil;
	b->said = false;
	b->next = b->slot = 0;
	GLS(a)->scan_direct = false;
}

/* gl_bridge_scan_close() for a context that could not be made current:
 * the slots' GL names and fence go with it undeleted -- kept, a slot
 * would hand the next context names it never made -- and Metal's half,
 * which needs no context, is waited for and let go as there, or the
 * IOSurfaces outlive the bridge, which no frame may come to remake. */
void gl_bridge_scan_forget(App *a)
{
	FnlCglBridge *b = bridge(a);
	if (!b)
		return;
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (b->cb[i])
			[b->cb[i] waitUntilCompleted];
		b->cb[i] = nil;
		b->fbo[i] = b->tex[i] = 0;
		b->mtex[i] = nil;
		if (b->surf[i])
			CFRelease(b->surf[i]);
		b->surf[i] = NULL;
		b->w[i] = b->h[i] = 0;
	}
	b->written = NULL;
	b->queue = nil;
	b->device = nil;
	b->said = false;
	b->next = b->slot = 0;
	GLS(a)->scan_direct = false;
}

/* ---- the GPUs ------------------------------------------------------------ */

/* The list is the one GPU in use, which device.c names as GL names it. */
int gl_bridge_gpus(App *a, GpuEntry *out, int *cur)
{
	(void)a; (void)out;
	*cur = -1;
	return 0;
}

/* The GPU a context of this pixel format draws on, as GL names it, while
 * another renderer draws: a context made, asked and let go, with the
 * thread's own current context put back. None where macOS gives no 4.1. */
int gl_bridge_list_gpus(App *a, GpuEntry *out, int max)
{
	(void)a;
	if (max < 1 || !gl_lib())
		return 0;
	CGLPixelFormatObj pf = pixel_format();
	if (!pf)
		return 0;
	CGLContextObj ctx = NULL, was = CGLGetCurrentContext();
	int n = 0;
	if (CGLCreateContext(pf, NULL, &ctx) == kCGLNoError && ctx &&
		CGLSetCurrentContext(ctx) == kCGLNoError) {
		typedef const GLubyte *(*GetString)(GLenum);
		const GetString get = (GetString)gl_bridge_proc("glGetString");
		const char *r = get ? (const char *)get(GL_RENDERER) : NULL;
		snprintf(out[0].name, sizeof out[0].name, "%s",
				 r ? r : "(unnamed)");
		out[0].kind = r && gl_renderer_is_software(r)
						  ? FNL_GPU_KIND_SOFTWARE : FNL_GPU_KIND_UNKNOWN;
		out[0].renderer = FNL_RENDERER_OPENGL;
		n = 1;
	}
	CGLSetCurrentContext(was);
	if (ctx)
		CGLReleaseContext(ctx);
	CGLReleasePixelFormat(pf);
	return n;
}
