/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Everything about the OpenGL renderer that no file outside src/renderer/gl/
 * may see: its state, behind App::renderer_state. gl_internal.h includes it;
 * nothing else does.
 *
 * App::renderer_state is allocated by create_instance() and freed by
 * destroy_instance(), so it exists from the first entry of the seam to the
 * last: every other FnlRendererVtbl entry may read it without a test. The few
 * entries callable from [any] thread -- device_name, present_mode_name,
 * max_samples -- read only App's own atomics, and App::gpu_name under
 * App::lock, and never it.
 *
 * GL names are GLuint, which is unsigned int; they are declared so here
 * rather than through <GL/glcorearb.h>, which gl_funcs.h brings for the
 * entry-point types alone; a name of zero is "none", as GL has it.
 */

#ifndef FNL_GL_STATE_H
#define FNL_GL_STATE_H

#include "renderer/gl/gl_funcs.h"

/* The renderer's state, from an App. */
#define GLS(a) ((a)->renderer_state)

/* The GPUs a display for this window can be opened on (EGLDeviceEXT, a
 * pointer too), their names and which are a software rasteriser; and those
 * a display opened on that made no 4.5 core context, left out of the list,
 * which EGL's own choice, with no device named, can still be
 * (gl_bridge_open()). The renderer's own, and a listing's while another
 * renderer draws (gl_bridge_list_gpus()). */
typedef struct {
	void *dev[GPU_LIST_MAX];
	char name[GPU_LIST_MAX][GPU_NAME_MAX];
	bool soft[GPU_LIST_MAX];
	int n;
	void *refused[GPU_LIST_MAX];
	int refused_n;
} GlDevices;

/* An image the GPU draws into: a framebuffer and what is attached to it.
 * All null together when there is none. `color` is a texture where the image
 * is sampled afterwards and a renderbuffer where it is only resolved, and
 * `sampled` says which: the two are separate namespaces, so the same number
 * can name one of each. */
typedef struct {
	unsigned fbo;
	unsigned color;
	unsigned depth; /* a texture where it is sampled, else a renderbuffer */
	bool sampled;
	bool depth_sampled;
} GlTarget;

/* The 4.1 profile's account of one vertex array (gl_compat.c): the format
 * each attribute was given, and the buffer its binding points at, kept
 * because 4.1 has no separate attribute format and no base instance. The
 * records are the window's, with its table: the names are its context's,
 * and the thread that made them is not always the one that draws. */
#define VAO_RECORDS 16
/* Five: the stroke's layout is packed as vec4s -- p0, p2, color, color2 and
 * shape_kind (overlay.c) -- and an attribute at or past this is silently
 * dropped here, leaving that one at the generic value. */
#define VAO_ATTRIBS 5
#define VAO_BINDINGS 1

typedef struct {
	bool      on;
	GLint     size;
	GLenum    type;
	GLboolean normalized;
	GLuint    offset;
	GLuint    binding;
} AttribRecord;

typedef struct {
	GLuint   buffer;
	GLintptr offset;
	GLsizei  stride;
	GLuint   divisor;
} BindingRecord;

typedef struct {
	GLuint        name;
	AttribRecord  attr[VAO_ATTRIBS];
	BindingRecord bind[VAO_BINDINGS];
} VaoRecord;

struct RendererState {
	/* ---- the bridge: the window system's half (egl.c) -----------------
	 * Every field of this half but `loaded` outlives a context, and is one
	 * forget_objects() (device.c) keeps: a field added here is a line
	 * there.
	 *
	 * EGLDisplay, EGLConfig, EGLSurface and EGLContext, which are all
	 * pointers, kept as such so that this header names no window system. */
	void *bridge_display;
	void *bridge_config;
	void *bridge_surface;
	/* EGL answered EGL_NOT_INITIALIZED for the display: terminated under
	 * the renderer by another library's eglTerminate on the same display
	 * -- NVIDIA's Vulkan driver does, at an instance's end -- which only a
	 * context already current survives. The next recovery opens it again
	 * (recover_from_loss()). */
	bool display_gone;
	void *bridge_context;
	/* What the native window was made as, where egl_<backend>.c makes one
	 * (Wayland's wl_egl_window); NULL where the window server's own window
	 * is handed over as it is. */
	void *bridge_native;
	/* The wl_egl_window's size, where the bridge makes one. A resize to
	 * the same size is not handed over (egl_wayland.c): Mesa marks the
	 * drawable dirty and allocates again. Zero where there is none. */
	int native_w, native_h;
	/* What eglCreatePlatformWindowSurface was handed, kept for a surface
	 * made again over the same native window (gl_bridge_resize()). */
	void *bridge_window;
	/* The GPUs the window's display can be opened on: found once, at the
	 * first open (egl.c, list_devices()), and kept for the instance's
	 * life, every move included. dev_cur is the one the display is on, -1
	 * where EGL could not say; devs.n 0 where EGL lists no devices, and the
	 * list is then the one GPU in use. */
	GlDevices devs;
	int dev_cur;
	bool dev_listed;
	/* The surface was made with EGL_PRESENT_OPAQUE_EXT, so its alpha is
	 * ignored; and whether the display offers that at all. Only asked where
	 * the configuration has alpha, which is where the window's
	 * transparency is the buffer's (EglBridge::alpha). */
	bool opaque;
	bool can_opaque;
	/* The context was made with a reset notification, so a lost device is
	 * reported (glGetGraphicsResetStatus) rather than undefined. */
	bool robust;
	/* The entry points were loaded for this context and it was current when
	 * they were. Without it no GL call is made, teardown's included: a
	 * context can exist with nothing loaded, when making it current or the
	 * version check failed. */
	bool loaded;
	/* The window's entry points (gl_funcs.h), loaded with its context, and
	 * whether the table is the 4.1 profile's. A table is the window's: a
	 * host of two windows may draw through two drivers' entry points, and a
	 * second window's load must not write under the first's draws. The
	 * calling thread's is gl_internal.h's `gl` (gl_bind()). */
	GlFuncs funcs;
	bool funcs_41;
	/* The 4.1 profile's standing state (gl_compat.c): the account of each
	 * vertex array of this window's context, and the driver's own
	 * DeleteVertexArrays its stand-in gives them back through. The window's
	 * with the table, so a device built on one thread and drawn on another
	 * finds them. */
	VaoRecord vaos[VAO_RECORDS];
	PFNGLDELETEVERTEXARRAYSPROC real_delete_vertex_arrays;
	/* The swap interval the surface was last given: 1 waits for the vertical
	 * blank and 0 does not; -1 for a surface given none yet, whatever EGL's
	 * default is. And the one vsync asks for, 1 or 0, mirrored into
	 * App::present_mode for [any]: what is given is 0 where something else
	 * paces, or during a resize (gl_choose_interval()). */
	int interval;
	int interval_asked;
	/* Whether the interval last refused has been logged: a driver that will
	 * not take one is asked again every frame (gl_choose_interval()), and
	 * one line a frame is a flood. Cleared when an ask is taken. */
	bool interval_said;
	/* When the window's image last changed size, on now_seconds()'s clock. */
	double resized_at;

	/* ---- the device ---------------------------------------------------- */
	/* A CPU rasteriser: its swap waits for the frame being drawn, so the
	 * block is work and App::present_block is left unmeasured. */
	bool software;

	/* The window's image: what the frame is drawn into, at App::extent, row
	 * 0 at the top as in Vulkan, and copied flipped onto the window's own
	 * framebuffer at the end of the frame (DESIGN *One shader source*). It
	 * stands where a Vulkan swapchain image does, and is remade where a
	 * swapchain would be. */
	GlTarget window;

	/* What the scene is rendered into at one resolution, a texture the
	 * fetch samples, with a depth renderbuffer of its own; and, built on
	 * first use, the multisampled pair FSAA draws into and resolves from,
	 * and MLAA's output. A refusal is remembered as Vulkan's is. */
	GlTarget scene;
	GlTarget scene_ms;
	bool scene_ms_refused;
	GlTarget mlaa;
	bool mlaa_refused;
	/* TAAU's resolve and the history it mixes in, one per frame slot: the
	 * fetch samples the resolve, the resolve samples the history, and the
	 * history is filled by a blit from the resolve -- not by the pass, which
	 * would be reading the image it writes. */
	GlTarget taau;
	GlTarget taau_hist[MAX_FRAMES_IN_FLIGHT];
	bool taau_hist_ok[MAX_FRAMES_IN_FLIGHT];
	bool taau_refused;
	uint64_t taau_present;

	/* The framebuffer on_record is drawing into, while it runs, and 0
	 * outside it; fnl_gl_scene_framebuffer() reads it back. */
	unsigned scene_fbo_now;

	/* ---- programs and their inputs --------------------------------------
	 * One program per pairing gl_program() links at build time. */
	unsigned prog_rect;
	unsigned prog_shape;
	unsigned prog_text;
	unsigned prog_blit;
	unsigned prog_mlaa;
	/* The resolve: the fetch's vertex stage and a fragment stage reading two
	 * images, so a program of its own. */
	unsigned prog_taau;
	/* The picture placed at a rectangle (fnl_overlay_image()); its texture is
	 * the caller's, behind FnlImage::gpu. */
	unsigned prog_image;
	/* One `Push` block per frame slot: the block is bound to point 0 for
	 * every program that has one, and it is rewritten as the slot records --
	 * glPush* would be per draw, but a UBO's contents are undefined for draws
	 * issued before a modification (GL 4.5 6.1.3), and a slot's draws may
	 * still be in flight when the next turn records into the other. The
	 * arenas are per-slot for the same reason. */
	unsigned push[MAX_FRAMES_IN_FLIGHT];
	/* The instance layouts, each fed from the frame slot's arena at the
	 * offset a flush reads from; and one with nothing in it, which a core
	 * context needs bound for the attribute-less fetch. */
	unsigned vao_rect;
	unsigned vao_shape;
	unsigned vao_text;
	unsigned vao_empty;

	/* GL_MAX_CLIP_DISTANCES of this context, each of which
	 * gl_own_state() turns off: one a caller left on clips the overlay. */
	int clip_distances;

	unsigned sampler_nearest; /* the atlas: baked for the pixel grid      */
	unsigned sampler_linear;  /* the scene: the whole filter of the scale */
	unsigned font_tex;
	/* The cover: App::snap, the frame read back before a move, as a
	 * texture of this context, and the App::snap_gen it was uploaded from
	 * or last tried for (gl_overlay_cover()). */
	unsigned cover_tex;
	uint32_t cover_gen;

	/* Per frame slot: the overlay's arenas, persistently mapped (App::
	 * text_map and App::ovl_map point into them), and the fence of the last
	 * frame that read them. A slot is reused only once its fence has
	 * signalled, which is what keeps the core from writing an arena the GPU
	 * may still be reading. */
	unsigned text_buf[MAX_FRAMES_IN_FLIGHT];
	unsigned ovl_buf[MAX_FRAMES_IN_FLIGHT];
	/* Under 4.1 the arenas are client memory and the buffers are filled
	 * by each flush: no persistent mapping there (overlay.c). One block
	 * for each kind's slots, NULL under 4.5; not the context's, so a lost
	 * context's forget_objects() frees them. */
	bool arenas_client;
	void *text_block;
	void *ovl_block;
	void *fence[MAX_FRAMES_IN_FLIGHT];
	/* Two GL_TIMESTAMP queries per slot, at the first and the last command
	 * of its frame, for fnl_frame_gpu_ms(); whether the slot's pair has
	 * been written since it was read (gl_time_read()), and by which frame
	 * (app_gpu_timed()). 0 before they are made. */
	unsigned ts_query[MAX_FRAMES_IN_FLIGHT][2];
	bool ts_unread[MAX_FRAMES_IN_FLIGHT];
	uint64_t ts_index[MAX_FRAMES_IN_FLIGHT];

	/* Whether the overlay is being drawn, set around draw_overlay(): a flush
	 * outside it draws nothing, as one outside a Vulkan recording does. */
	bool overlay_on;
	/* The core's scissor box for the overlay's deferred draws, as it last
	 * asked through gl_overlay_scissor(): a caller may set a box of its own
	 * with the context current in on_overlay, and every flush puts this one
	 * back (overlay_own_state()). Cleared -- false -- is the whole image,
	 * which is what draw() sets before the callback. */
	bool    ovl_scissor_clip;
	FnlRect ovl_scissor;

	/* A surfaceless bridge's frame, read back for the backend: malloc'd,
	 * not a GL name, grown to the largest window image and freed with the
	 * device objects. */
	void *readback;
	size_t readback_bytes;
	/* The copy path's readback, through a pack buffer: started before the
	 * frame's end stamp, so the GPU's copy is in its time on the frame,
	 * and mapped by the present (pack_start(), gl_present_pixels()). Its
	 * size, the extent read into it, and whether this frame's is waiting
	 * to be mapped. 0 before the first. */
	unsigned pack_buf;
	size_t pack_bytes;
	int pack_w, pack_h;
	bool pack_pending;

	/* KMS: the output's own buffers, drawn into by the GPU in place of the
	 * copy path's readback (egl_kms.c). For each of the backend's pair, the
	 * EGL image it was imported as (EGLImage, kept as a pointer so this
	 * header names no EGL), the renderbuffer and framebuffer over it, and
	 * the backend's key for the buffer; the buffer this frame went into,
	 * -1 for none; whether this frame went that way; whether the way is
	 * given up on this device; and whether which way was said. */
	void *scan_img[2];
	unsigned scan_rb[2], scan_fbo[2];
	/* A step that may fail by design is being tried, and what the debug
	 * output says of it is no defect: left out of the log (device.c). */
	bool debug_quiet;
	uint32_t scan_key[2];
	int scan_back;
	bool scan_direct, scan_off, scan_said;
};

#endif /* FNL_GL_STATE_H */
