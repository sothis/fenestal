/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the files of the Vulkan renderer share with each other and with
 * nothing else. src/core/ and src/backend/ reach this directory through
 * FnlRendererVtbl (core/renderer.h) and through the few overlay entry points
 * core/app.h declares. The renderer's state is in vk_state.h, behind
 * App::renderer_state.
 *
 * Included after "core/app.h", which every .c names first.
 */

#ifndef FNL_VK_INTERNAL_H
#define FNL_VK_INTERNAL_H

#include "core/app.h"
#include "renderer/vulkan/vk_state.h"
#include "renderer/vulkan/vk_fns.h"

/* Classify a VkResult, recording the call that produced it. FNL_OK when it
 * succeeded, so this wraps a call rather than following one. */
FNL_MUST_USE FnlResult fnl_vk_result(const char *expr, const char *file,
									 int line, VkResult r);  /* device.c */

/* Wrap a Vulkan call as an FnlResult.
 *
 * Each macro below stringifies its own argument rather than hand it to
 * VKR: an argument passed on is expanded first, and the message would name
 * vk_fns.h's redirect, (*fnl_vkFoo), where the source says vkFoo. */
#define VKR(expr) fnl_vk_result(#expr, __FILE__, __LINE__, (expr))

/* A Vulkan call that must succeed, checked and propagated. */
#define VK_TRY(expr)                                                          \
	FNL_TRY(fnl_vk_result(#expr, __FILE__, __LINE__, (expr)))

/* The same, for the builders whose result *is* a Vulkan handle: they report
 * failure as VK_NULL_HANDLE, with the message still recorded. */
#define VK_TRY_NULL(expr)                                                     \
	do {                                                                      \
		if (fnl_vk_result(#expr, __FILE__, __LINE__, (expr)) < 0)             \
			return VK_NULL_HANDLE;                                            \
	} while (0)

/* And for the frame's entry points that answer `bool`.
 *
 * VK_TRY must never be used in a bool function: `return fnl_try_r_` there
 * converts a negative FnlResult to `true`, so every failure is reported as
 * success, and no warning says so. */
#define VK_TRY_FALSE(expr)                                                    \
	do {                                                                      \
		if (fnl_vk_result(#expr, __FILE__, __LINE__, (expr)) < 0)             \
			return false;                                                     \
	} while (0)

/* The bridge to one backend, from surface_<backend>.c: a table, because
 * a Linux build has one per backend and learns which at open (DESIGN *One
 * build per operating system*). make_instance() finds the one for
 * FnlBackendVtbl::id and keeps it in RendererState::bridge.
 *
 * instance_extensions is every instance extension the bridge wants beside
 * VK_KHR_surface, at most VK_BACKEND_EXTENSIONS_MAX of them, the first
 * `*required` of which the instance cannot do without. The rest are enabled
 * where the loader has them, and RendererState::backend_ext_on says which
 * were: one required for a window server; for a bare display, two ways to
 * reach it and none required, since either will do.
 *
 * create_surface makes RendererState::vk_surface, at startup and again after
 * the surface is lost.
 *
 * can_draw: whether a device can draw this window, whether or not the
 * surface made now is its -- on KMS by taking the output's display or by
 * the copy path, either of which a move to it takes (surface_kms.c) -- so
 * that pick_device() lists and numbers those, and chooses among the ones
 * the surface takes. NULL where the list is the devices that can present
 * to the surface. */
#define VK_BACKEND_EXTENSIONS_MAX 6
typedef struct VkBridge {
	BackendId backend;
	const char *const *(*instance_extensions)(uint32_t *count,
											  uint32_t *required);
	FNL_MUST_USE FnlResult (*create_surface)(App *a);
	bool (*can_draw)(App *a, VkPhysicalDevice dev);
	/* Whether a queue family of `dev` presents to this window server, asked
	 * with no surface (vkGetPhysicalDevice*PresentationSupportKHR): how the
	 * GPUs are listed while another renderer draws (vk_list_gpus()). NULL
	 * where the window server has no such question, and every device with
	 * a graphics queue is listed -- a Mac's, whose layer any device draws
	 * into. can_draw answers first where there is one. Both are asked off
	 * the render thread, of an instance that is not the renderer's, so
	 * neither reads VK(a). */
	bool (*presents)(App *a, VkInstance inst, VkPhysicalDevice dev,
					 uint32_t family);
	/* Before the live swapchain is destroyed, where the kernel scans its
	 * images out with no window server between: the backend puts a frame
	 * of its own on the output first, so the images are on no plane when
	 * the driver removes their framebuffers (kms_scanout_take()). NULL on
	 * a window server, whose compositor owns what is shown. */
	void (*scanout_take)(App *a);
	/* Where the window can be images of the renderer's own that reach the
	 * window server another way (RendererState::dxgi, present_dxgi.c):
	 * `choose_present` decides, after the device is made and before
	 * anything is built for the window; `own_images` makes them in place
	 * of own_images_make(), `submit_chain` is what a frame's submit
	 * carries for them, `present_own` hands one over once its frame is
	 * submitted, `own_end`
	 * lets go of everything at teardown, and `device_extensions` is what
	 * the device needs for them, each enabled where offered. NULL
	 * elsewhere. */
	FnlResult (*choose_present)(App *a);
	/* Whether `dev` can draw this window that way, asked of any instance
	 * the bridge's extensions were asked for -- the renderer's own, or the
	 * one vk_list_gpus() makes while another renderer draws -- so it reads
	 * neither App::renderer_state nor anything else of the renderer's.
	 * Such a device is listed twice, the second entry that way
	 * (GpuEntry::via). */
	bool (*shares)(VkInstance inst, VkPhysicalDevice dev);
	FnlResult (*own_images)(App *a, VkExtent2D e, uint32_t n);
	const void *(*submit_chain)(App *a, uint32_t img);
	VkResult (*present_own)(App *a, uint32_t img);
	void (*own_end)(App *a);
	const char *const *(*device_extensions)(uint32_t *count);
	/* Where the driver makes the flips and nothing else says when a frame
	 * reached the screen -- Vulkan's own display path -- whether the
	 * driver is to be asked, by VK_EXT_present_timing (frame.c,
	 * timing_take()): true once the instance has what the extension
	 * depends on, the bridge's to enable. NULL elsewhere. */
	bool (*driver_stamps)(App *a);
	/* A present time the driver gives by VK_GOOGLE_display_timing, in its
	 * nanoseconds, on now_seconds()'s clock: that extension names no
	 * clock, so the bridge, which knows whose driver it is, says. Asked
	 * for only where VK_EXT_present_timing, which names its clock, is not
	 * to be had. NULL where the bridge does not know the driver's clock,
	 * and the extension is then not used. */
	double (*driver_clock)(App *a, uint64_t ns);
	/* Whether the window server has said the window is covered, where it
	 * says so and a present there is one to skip
	 * (FnlRendererVtbl::occluded): AppKit's occlusion state on a Mac, read
	 * as Metal's and OpenGL's bridges read it. NULL elsewhere, where the
	 * answer is no. */
	bool (*occluded)(const App *a);
} VkBridge;

/* How many presents the driver keeps the times of until they are read
 * (VK_EXT_present_timing): read each frame, a queue of a few frames is
 * room enough, and a present that would fill it goes unasked rather than
 * failing (frame.c, timing_take()). */
#define TIMING_QUEUE 16
/* How often a present stage's own clock is calibrated against
 * CLOCK_MONOTONIC again (vk_timing_calibrate()): a second, so a drift of
 * even a hundred parts per million stays a tenth of a microsecond. */
#define TIMING_CALIBRATE_S 1.0
/* The present stage's clock against CLOCK_MONOTONIC, by one calibrated
 * pair of readings (RendererState::timing_offset_ns). False where the
 * device would not read both. swapchain.c. */
bool vk_timing_calibrate(App *a);

/* A graphics queue family of `dev` that presents to `surface`, or -1; with
 * no surface (RendererState::surfaceless), any graphics queue family. */
int vk_find_present_queue(VkPhysicalDevice dev, VkSurfaceKHR surface);

/* The layout the window's pass leaves its image in: the swapchain's present
 * layout, or where the window is images of the renderer's own
 * (RendererState::surfaceless), the readback's -- the present layout is a
 * swapchain image's alone. GENERAL ahead of both where those images are
 * shared with Direct3D 11 (RendererState::dxgi), which reads them outside
 * Vulkan. */
static inline VkImageLayout vk_window_layout(const App *a)
{
	if (a->renderer_state->dxgi)
		return VK_IMAGE_LAYOUT_GENERAL;
	return a->renderer_state->surfaceless
			   ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
			   : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

/* FNL_VK_DXGI: 1 or 0 where it says, -1 where it does not (device.c). */
int         vk_dxgi_env(void);
#if defined(_WIN32)
FnlResult   vk_dxgi_choose(App *a);
bool        vk_dxgi_shares(VkInstance inst, VkPhysicalDevice dev);
const char *const *vk_dxgi_device_extensions(uint32_t *count);
FnlResult   vk_dxgi_own_images(App *a, VkExtent2D e, uint32_t n);
const void *vk_dxgi_submit_chain(App *a, uint32_t img);
VkResult    vk_dxgi_present(App *a, uint32_t img);
void        vk_dxgi_end(App *a);
#endif

extern const VkBridge vk_bridge_wayland;
extern const VkBridge vk_bridge_x11;
extern const VkBridge vk_bridge_kms;
extern const VkBridge vk_bridge_win32;
extern const VkBridge vk_bridge_cocoa;

/* How the pipelines make_overlay_pipeline() builds differ from one another;
 * everything else about them is identical. */
typedef enum {
	OVERLAY_BLEND_NONE,      /* rectangles: they replace a clear, so write */
	OVERLAY_BLEND_ALPHA,     /* strokes, and text without dual-source      */
	OVERLAY_BLEND_DUAL_SRC,  /* subpixel text: a weight per channel        */
	OVERLAY_BLEND_CONSTANT,  /* the cover: one weight a draw, set dynamic  */
} OverlayBlend;

/* What tells the renderer's three render passes apart; make_render_pass()
 * holds everything they share. */
typedef struct {
	VkSampleCountFlagBits samples;  /* above one adds a resolve attachment   */
	bool depth;                     /* a depth attachment, never stored      */
	bool clear;                     /* clear the colour, or leave it to be
									 * overwritten in full                   */
	bool sampled;                   /* the result is fetched by a later pass
									 * rather than presented                 */
} PassDesc;

/* Push constants for the overlay pipelines: what a frame's primitives share,
 * everything per primitive being an instance attribute. Each must match its
 * GLSL block byte for byte (std430); a mismatch is garbled geometry at run
 * time, not a compile error, and the asserts pin every offset padding could
 * move. */
typedef struct {
	float screen[2];
} OverlayPush;

_Static_assert(sizeof(OverlayPush) == 8,
			   "OverlayPush must match shaders/*.vert");

/* The text pipeline also tells the fragment stage which space this surface's
 * blend works in (see shaders/text.frag) and how wide a texel is, which the
 * subpixel path steps by. */
typedef struct {
	float    screen[2];
	uint32_t srgb;
	float    texel_u;      /* 1 / atlas width, in normalised units */
} TextPush;

_Static_assert(sizeof(TextPush) == 16, "TextPush must match shaders/text.*");
_Static_assert(offsetof(TextPush, srgb) == 8, "TextPush layout drifted");
_Static_assert(offsetof(TextPush, texel_u) == 12, "TextPush layout drifted");

/* App::msaa_samples, typed. */
static inline VkSampleCountFlagBits scene_samples(const App *a)
{
	return (VkSampleCountFlagBits)fnl_atomic_load_u32(&a->msaa_samples);
}

/* The present mode the swapchain that exists was built with. Every value ever
 * stored here is one of the three the spec numbers 0 to 2, so the round trip
 * through uint32_t is exact. */
static inline VkPresentModeKHR vk_present_mode(const App *a)
{
	return (VkPresentModeKHR)fnl_atomic_load_u32(&a->present_mode);
}

static inline void vk_set_present_mode(App *a, VkPresentModeKHR m)
{
	fnl_atomic_store_u32(&a->present_mode, (uint32_t)m);
}

/* The mode a swapchain should be built with. Vsync is FIFO, the one mode the
 * spec guarantees every surface. */
static inline VkPresentModeKHR vk_wanted_present_mode(const App *a, bool vsync)
{
	return vsync ? VK_PRESENT_MODE_FIFO_KHR : VK(a)->mode_tearing;
}

/* ---- vk_load.c ----------------------------------------------------------- */

/* The loader opened, once per process, and vk_fns.h's global entries taken
 * from it: before anything else of Vulkan's is called. FNL_ERR_NO_DEVICE
 * where there is none, which fnl_init() steps past. */
FNL_MUST_USE FnlResult vk_load_global(void);
/* Every other entry of vk_fns.h, from the instance just made. A core entry
 * missing is FNL_ERR_NO_DEVICE, and vkDestroyInstance, the first asked, is
 * then this instance's or NULL. */
FNL_MUST_USE FnlResult vk_load_instance(VkInstance instance);

/* ---- device.c ------------------------------------------------------------ */

/* Classify a result that says the objects underneath us are gone, recording
 * which. True means the caller should unwind rather than treat it as fatal. */
bool vk_lost(App *a, VkResult r);
/* The constant's own name, for a message; device.c. */
const char *vk_result_str(VkResult r);

/* ---- pipeline.c ---------------------------------------------------------- */

FNL_MUST_USE FnlResult create_pipeline_cache(App *a);
void save_pipeline_cache(App *a);

VkShaderModule make_shader(App *a, const uint32_t *code, size_t bytes);
/* One of the three passes, or VK_NULL_HANDLE with the failure recorded. */
VkRenderPass make_render_pass(App *a, const PassDesc *d);

/* A quad or a covering triangle built from gl_VertexIndex, single-sampled,
 * dynamic viewport and scissor, no depth. With `n_va` attributes it is a
 * four-vertex strip taking one instance per primitive from a binding of
 * `stride` bytes; with none it is a three-vertex list and nothing is bound.
 *
 * A pipeline may only be bound inside a pass compatible with `pass`, which
 * includes the subpass dependencies, so the same shaders are built once per
 * pass they run in. */
VkPipeline make_overlay_pipeline(App *a, VkRenderPass pass,
								 VkShaderModule vert, VkShaderModule frag,
								 VkPipelineLayout layout, uint32_t stride,
								 const VkVertexInputAttributeDescription *va,
								 uint32_t n_va, OverlayBlend blend);

FNL_MUST_USE FnlResult find_memory_type(App *a, uint32_t bits,
										VkMemoryPropertyFlags props,
										uint32_t *out);

/* A 2D view of the whole of `image`, and a one-layer framebuffer. Both hand
 * back the VkResult and leave *out null on failure, so a caller for whom the
 * failure is fatal wraps the call in VK_TRY and one that degrades compares. */
VkResult make_image_view(App *a, VkImage image, VkFormat fmt,
						 VkImageAspectFlags aspect, VkImageView *out);
VkResult make_framebuffer(App *a, VkRenderPass pass, uint32_t n_views,
						  const VkImageView *views, uint32_t w, uint32_t h,
						  VkFramebuffer *out);

/* An attachment the renderer owns: image, memory and view, or nothing at all
 * and false. `lazy` asks for transient, lazily allocated storage where the
 * device has it, for an attachment that is never stored. Nothing is logged:
 * every caller but one answers a refusal by drawing without the target. */
bool create_target_image(App *a, uint32_t w, uint32_t h, VkFormat fmt,
						 VkSampleCountFlagBits samples,
						 VkImageUsageFlags usage, bool lazy, TargetImage *t);
/* Destroys and nulls; safe on one that was never built. */
void destroy_target_image(App *a, TargetImage *t);

/* `count` buffers behind one VkDeviceMemory and one mapping, which is why the
 * memory handle is singular while the other two are arrays. Host-visible and
 * coherent always; `prefer` is a property more asked for where the device has
 * it beside those two, VK_MEMORY_PROPERTY_HOST_CACHED_BIT for memory the CPU
 * reads, 0 for memory it only writes. */
FNL_MUST_USE FnlResult create_instance_buffers(App *a, VkDeviceSize bytes,
								  VkBufferUsageFlags usage,
								  VkMemoryPropertyFlags prefer, uint32_t count,
								  VkBuffer *buf, VkDeviceMemory *mem,
								  void **map);
void destroy_instance_buffers(App *a, uint32_t count, VkBuffer *buf,
							  VkDeviceMemory *mem, void **map);

/* Synchronous one-shot submit, for setup transfers. */
VkCommandBuffer begin_oneshot(App *a);
FNL_MUST_USE FnlResult end_oneshot(App *a, VkCommandBuffer cb);

/* ---- swapchain.c --------------------------------------------------------- */

/* The window's pass, built once with the device. */
FNL_MUST_USE FnlResult create_render_pass(App *a);
/* The swapchain and everything per image of it, framebuffers included. A
 * failure leaves whatever was built for destroy_swapchain(). */
FNL_MUST_USE FnlResult create_swapchain(App *a, VkSwapchainKHR old_swapchain);
void destroy_swapchain(App *a);
bool recreate_swapchain(App *a);
bool vk_present_mode_matches(const App *a, bool vsync);
void vk_retire_tick(App *a);
void free_all_retired(App *a);
void reap_stop(App *a);

/* ---- frame.c: the frame that acks, waited for until shown ---------------- */

/* Until the waiter is done with the frame handed to it. */
void shown_idle(App *a);
/* Ahead of anything that ends the device: an ack still held is released
 * and the thread is gone when this returns. */
void shown_stop(App *a);

/* ---- scene_target.c ------------------------------------------------------ */

/* The scene's passes, the fetch and filter pipelines and the sampler they
 * share. Built once with the device; the images come and go below. */
FNL_MUST_USE FnlResult create_scene_objects(App *a);
void destroy_scene_objects(App *a);
/* Resize the offscreen scene image, or drop it for SCENE_NATIVE. A no-op when
 * the size already matches. Drains the queue first when there is something to
 * replace: command buffers that may still be executing name the image and its
 * descriptor. Returns nothing: a lost device and a refused target both leave
 * scene_extent at whatever exists, which is what every reader uses. */
void scene_set_size(App *a, int w, int h);
/* Build the half of the target a frame's antialiasing needs and does not have
 * yet: the multisampled pair, or the filter's output image. True when
 * something was built, which the caller answers by re-recording. */
bool scene_ensure_aa(App *a, bool want_ms, bool want_mlaa);
/* The seam's overlay_cover: App::snap over the window at `alpha`. */
bool vk_overlay_cover(App *a, float alpha);
/* The seam's cover_release: the cover and the snapshot's buffer let go. */
void vk_cover_release(App *a);

/* ---- text.c, overlay.c --------------------------------------------------- */

FNL_MUST_USE FnlResult create_text_pipeline(App *a);
void destroy_text_pipeline(App *a);
/* Swap in the atlas baked for the nearest scale to `s120`, if that is not the
 * one already up. */
FNL_MUST_USE FnlResult text_set_scale(App *a, int s120);
FNL_MUST_USE FnlResult create_overlay_pipelines(App *a);
void destroy_overlay_pipelines(App *a);

/* ---- frame.c ------------------------------------------------------------- */

bool draw_frame(App *a, uint32_t frame, const FnlDrawState *ds,
				uint32_t cfg_serial, bool cfg_pending);
int  vk_max_samples(const App *a);
int  vk_active_samples(const App *a, const FnlDrawState *ds);
void vk_gpu_time_poll(App *a);

/* The overlay's draws, the seam's last four entries. */
void vk_overlay_flush_rects(App *a);
void vk_overlay_flush_shapes(App *a);
void vk_overlay_scissor(App *a, const FnlRect *r);
FNL_MUST_USE FnlResult vk_text_end(App *a);

#endif  /* FNL_VK_INTERNAL_H */
