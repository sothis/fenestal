/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Everything about the Vulkan renderer that no file outside
 * src/renderer/vulkan/ may see: its state, behind App::renderer_state, and
 * the types and limits that size it. vk_internal.h includes it; nothing else
 * does.
 *
 * App::renderer_state is allocated by create_instance() and freed by
 * destroy_instance(), so it exists from the first entry of the seam to the
 * last: every other FnlRendererVtbl entry may read it without a test. The few
 * entries callable from [any] thread -- device_name, present_mode_name,
 * max_samples -- read only App's own fields, atomics or under App::lock,
 * and never it. The public interop (fnl_vulkan.h) reads it, so its entries
 * are the render thread's, from the callbacks, and each tests
 * renderer_state first.
 */

#ifndef FNL_VK_STATE_H
#define FNL_VK_STATE_H

#include <vulkan/vulkan.h>

/* The renderer's state, from an App. */
#define VK(a) ((a)->renderer_state)

/* The renderer's state for the host, from an App
 * (App::host->renderer_host): one VkInstance and one VkDevice for every
 * window on it. A second window is a second surface and swapchain on that
 * device, never a device of its own -- Intel's driver loses its device again
 * and again with two of them open on one GPU, and its fifth creation after
 * the losses faults. The first window to arrive makes them, the last to leave
 * gives them up, and every window between borrows them. */
struct VkHost {
	/* The host's lock, and under it: the windows attached (create_instance()
	 * to destroy_instance()) and the windows with objects live on the
	 * device. The last of the second kind out destroys the device, and no
	 * device is made while one of them is still on a dead one
	 * (create_device_objects()). */
	FnlMutex lock;
	int      users;
	int      live;
	/* Bumped by each device made. RendererState::generation is the one a
	 * window's objects were built on: a window still on `lost_gen` is on a
	 * device that went, and rebuilds -- the loss is every window's, since
	 * the device is. Not cleared by the rebuild: the new device's generation
	 * is a different number, which is what ends the loss. */
	uint32_t     generation;
	FnlAtomicU32 lost_gen;

	/* The instance, and what make_instance() settled about it. */
	VkInstance  instance;
	const struct VkBridge *bridge;
	uint32_t    backend_ext_on;
	bool        surface_maint;
	PFN_vkGetPhysicalDeviceFeatures2KHR get_features2;
	PFN_vkGetPhysicalDeviceMemoryProperties2KHR get_memory2;
	PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR surface_caps2;

	/* The device, what pick_device() chose and create_device() made. */
	VkPhysicalDevice phys;
	uint32_t         queue_family;
	bool             phys_software;
	bool             phys_vram_unkept;
	VkDevice         device;
	VkQueue          queue;
	char             gpu_name[GPU_NAME_MAX];
	uint32_t         msaa_samples;
	/* Whether the device was made with VK_KHR/EXT_swapchain_maintenance1 and
	 * its feature. The device's, not a window's: one window's refused scale
	 * makes the device without it, and a window that borrows that device
	 * must not chain a scaled swapchain on it (create_device()). */
	bool             present_scale;
	/* The physical device's extension names, asked once for every window
	 * (device_has_extension()). */
	VkExtensionProperties *dev_exts;
	uint32_t               dev_ext_n;
	VkPhysicalDevice       dev_ext_phys;

	/* One queue, so its calls are serialised across the render threads:
	 * vkQueueSubmit() and vkQueuePresentKHR() are externally synchronised
	 * per queue, and vkDeviceWaitIdle() is every queue's. Taken by
	 * vk_queue_submit(), vk_queue_present(), vk_queue_wait_idle() and
	 * vk_device_wait_idle() alone. */
	FnlMutex queue_lock;
};

/* The host's Vulkan state, from an App: non-NULL from create_instance() to
 * destroy_instance(), which is the window's whole life on the renderer. */
#define VKH(a) ((struct VkHost *)(a)->host->renderer_host)

_Static_assert(GPU_NAME_MAX >= VK_MAX_PHYSICAL_DEVICE_NAME_SIZE,
			   "GPU_NAME_MAX must hold a VkPhysicalDeviceProperties name");

/* How many swapchains retired by a rebuild may be pending destruction at
 * once. Each is freed once MAX_FRAMES_IN_FLIGHT further frames have been
 * submitted (FnlRendererVtbl::retire_tick); a burst deeper than this forces an
 * early flush. */
#define MAX_RETIRED 4
/* Swapchains waiting for the reaper. A new one is retired every few frames
 * at most and destroyed in a third of that, so more than two waiting is a
 * reaper that has stopped; past this many they are destroyed in line. */
#define REAP_MAX 8

/* An image the renderer allocates for itself. All three are null together
 * when there is none. */
typedef struct {
	VkImage        image;
	VkDeviceMemory mem;
	VkImageView    view;
} TargetImage;

/* What a rebuild takes off the live swapchain and keeps until it is provably
 * idle: everything a command buffer in flight may still name. Copied by
 * value; entries past image_count are null. */
typedef struct {
	VkSwapchainKHR    swapchain;
	VkImageView       views[OVERLAY_MAX_IMAGES];
	VkFramebuffer     framebuffers[OVERLAY_MAX_IMAGES];
	VkSemaphore       render_done[OVERLAY_MAX_IMAGES];
	VkCommandBuffer   img_cmd[OVERLAY_MAX_IMAGES];
	/* A depth image the window grew out of while this was the newest
	 * entry, or null: the depth image outlives swapchains. */
	TargetImage       depth;
	/* The surfaceless window's images (RendererState::own), null where a
	 * swapchain's images were. */
	TargetImage       own[OVERLAY_MAX_IMAGES];
	/* HDR's per-image window images and encode framebuffers
	 * (RendererState::present, encode_fb), null in an SDR tree. */
	TargetImage       present[OVERLAY_MAX_IMAGES];
	VkFramebuffer     encode_fb[OVERLAY_MAX_IMAGES];
	uint32_t          image_count;
	uint32_t          countdown;
} RetiredSwapchain;

struct RendererState {
	/* This backend's surface_<backend>.c, from make_instance() on. */
	const struct VkBridge *bridge;
	VkInstance            instance;
	VkSurfaceKHR          vk_surface;
	/* Where the surface is one device's own -- a display plane, which is
	 * made on the device that drives it -- that device, and the display;
	 * VK_NULL_HANDLE where any device that can present may be picked. Set
	 * by the bridge, kept for the instance's life: a display is acquired
	 * once and survives a lost device. */
	VkPhysicalDevice      surface_phys;
	VkDisplayKHR          vk_display;
	/* Which of the bridge's optional instance extensions the loader had and
	 * the instance enabled, by their place in the bridge's list past the
	 * required ones (see VkBridge::instance_extensions). */
	uint32_t              backend_ext_on;
	/* Set by the bridge where no device can present to the output, and the
	 * frames are drawn on a headless surface instead: each image is copied
	 * into readback[img] at the end of its command buffer, and the pixels
	 * are handed to FnlBackendVtbl::present_pixels once the device has
	 * finished the frame. The buffers are device-scoped, like the instance
	 * buffers and for their reason, and sized for the largest extent yet. */
	bool                  present_copy;
	/* Set by the bridge with present_copy where the device presents to no
	 * surface at all -- NVIDIA's own driver has no headless surface -- and
	 * there is none: the frames are drawn into own[], images of the
	 * renderer's with no swapchain behind them, taken in turn (own_next)
	 * where a swapchain's would be acquired, and nothing is presented but
	 * the readback. Their views are views[], like a swapchain's; own[] holds
	 * the images and their memory. */
	bool                  surfaceless;
	TargetImage           own[OVERLAY_MAX_IMAGES];
	uint32_t              own_next;
	/* Set by the bridge where a swapchain the surface refuses means the
	 * surface will not show: a display mode whose test commit the kernel
	 * fails, on a selection that is not the last to have shown.
	 * create_swapchain() then reports the surface lost, so that it is made
	 * again and the selection given up (surface_kms.c); a retry of the
	 * same swapchain each turn would hold the window there for good. */
	bool                  swapchain_refusal_lost;
	VkImage               sc_images[OVERLAY_MAX_IMAGES];
	/* The live swapchain's images: App::extent, or larger where the
	 * window shows part of them (App::buffer, resize_in_place()). */
	VkExtent2D            sc_extent;
	VkBuffer              readback[OVERLAY_MAX_IMAGES];
	VkDeviceMemory        readback_mem;
	void                 *readback_map[OVERLAY_MAX_IMAGES];
	VkDeviceSize          readback_bytes;
	VkPhysicalDevice      phys;
	/* The device's generation (VkHost::generation) that this window's objects
	 * were built on: a window still on the generation a loss was reported at
	 * is drawing on a device that is gone, and rebuilds with every other. */
	uint32_t              generation;
	/* A CPU rasteriser: its present blocks on the frame being drawn, so the
	 * block is work and App::present_block is left unmeasured. */
	bool                  phys_software;
	/* A device whose VRAM does not keep a tiled image across its runtime
	 * suspend (device.c, device_passed_over()): text.c puts the atlas,
	 * uploaded once and kept, outside it. */
	bool                  phys_vram_unkept;
	uint32_t              queue_family;
	VkDevice              device;
	VkQueue               queue;
	/* Queried once with the device; find_memory_type() runs per allocation. */
	VkPhysicalDeviceMemoryProperties mem_props;

	/* The swapchain and what exists once per image of it. Fixed arrays,
	 * because create_swapchain() refuses more than OVERLAY_MAX_IMAGES; an
	 * entry past image_count, or one whose build failed, is null, which is
	 * what lets a half-built swapchain be destroyed. */
	VkSwapchainKHR        swapchain;
	uint32_t              image_count;
	VkImageView           views[OVERLAY_MAX_IMAGES];
	VkFramebuffer         framebuffers[OVERLAY_MAX_IMAGES];
	VkSemaphore           render_done[OVERLAY_MAX_IMAGES];
	/* The fence of the frame that last used each image. Not retired with
	 * the swapchain; see create_swapchain() for what a rebuild seeds it
	 * with. */
	VkFence               images_in_flight[OVERLAY_MAX_IMAGES];

	/* One command buffer per image, and the record_gen each was recorded
	 * at. Zero is never a live generation, so a new swapchain records
	 * every buffer on first use. */
	VkCommandBuffer       img_cmd[OVERLAY_MAX_IMAGES];
	uint32_t              img_cmd_gen[OVERLAY_MAX_IMAGES];
	/* The slot a recording was made in, and whether it was made with TAAU
	 * on. The resolve reads a history image per slot, so a recording reused
	 * in another slot would read the wrong one: under TAAU the slot is part
	 * of what a recording is for, and frame_record() re-records rather than
	 * submit one elsewhere. */
	uint32_t img_slot[OVERLAY_MAX_IMAGES];
	bool img_taau[OVERLAY_MAX_IMAGES];
	/* Whether the recording mixed its slot's history in (App::presents and
	 * taau_hist_ok as record_taau_pass() read them). Baked into the push
	 * constant, so a recording is only good while the answer stands: the
	 * first frame after a rest, a pause or a dropped present is recorded
	 * with no history, and reusing it once the accumulation may follow would
	 * hold the resolve at that frame for good. */
	bool img_taau_prev[OVERLAY_MAX_IMAGES];

	/* Two timestamps per image, at the first command of its buffer and
	 * after the copy path's readback, ahead of a snapshot's copy
	 * (record_command_buffer()), for fnl_frame_gpu_ms();
	 * VK_NULL_HANDLE where the queue keeps no time. ts_ns is the device's
	 * nanoseconds per tick and ts_mask the bits a timestamp has; ts_unread
	 * says an image's buffer ran since its pair was last read
	 * (gpu_time_read()), and ts_index which frame that run drew
	 * (app_gpu_timed()). */
	VkQueryPool           ts_pool;
	/* FNL_COST_PARTS's four more per image, each at the bottom of the
	 * pipe: at the start beside the pair's first, after the scene's pass,
	 * before the window's pass ends and after it (gpu_parts_read()). */
	VkQueryPool           ts_parts;
	double                ts_ns;
	uint64_t              ts_mask;
	bool                  ts_unread[OVERLAY_MAX_IMAGES];
	uint64_t              ts_index[OVERLAY_MAX_IMAGES];

	/* The window pass's depth attachment: one image, in every framebuffer of
	 * the swapchain. Sharing it is legal because it is cleared as the pass
	 * begins and never stored, and the pass's external dependency orders one
	 * frame's clear after the last one's depth writes. Kept across sizes
	 * while the extent fits, depth_w by depth_h, and retired with the
	 * newest swapchain when it is grown (depth_ensure()). */
	TargetImage           depth;
	uint32_t              depth_w, depth_h;
	VkFormat              depth_format;

	RetiredSwapchain      retired[MAX_RETIRED];
	int                   retired_count;
	/* When a swapchain was last replaced; see recreate_swapchain(). */
	double                swap_rebuilt_at;
	/* The render thread's, for swap_stale_note() in frame.c: the live
	 * swapchain has handed out no image yet, it was made at the surface's
	 * currentExtent, how many such swapchains in a row were out of date at
	 * their first acquire, and whether this run has asked for a nudge. */
	bool                  swap_fresh;
	bool                  swap_agreed;
	int                   swap_stale_run;
	bool                  swap_nudged;
	/* The live swapchain was said suboptimal or out of date, which only a
	 * new one answers: no size is then drawn into it in place
	 * (resize_in_place()). */
	bool                  swap_spent;
	/* The device can present a swapchain one to one from its corner, so
	 * an image larger than the window stays valid (swapchain.c). Cleared
	 * for this renderer's life when such a swapchain comes back out of
	 * date or suboptimal (`scale_refused`): the next one is the window's
	 * size, and a device made again does not turn it back on. `scale_said`
	 * is the one line that the capability was taken. */
	bool                  present_scale;
	bool                  scale_refused;
	bool                  scale_said;
	/* The instance enabled get_surface_capabilities2 and a surface
	 * maintenance extension, which is what the device half above needs.
	 * Set from the names that were enabled, so this file's device code
	 * does not learn which window server it presents to. */
	bool                  surface_maint;
	/* The window's images are shared with Direct3D 11 and presented
	 * through a DXGI swapchain (present_dxgi.c), whose state is
	 * `dxgi_state`; `surfaceless` is set with it. `dxgi_exts` is every
	 * extension that takes enabled on the device. */
	bool                  dxgi;
	bool                  dxgi_exts;
	/* The DXGI present's live swapchain: whether it allows tearing, and
	 * whether the factory offers it -- what vk_present_mode_matches()
	 * compares the policy against, since there is no Vulkan swapchain to
	 * ask (present_dxgi.c). */
	bool                  dxgi_tear, dxgi_tear_ok;
	void                 *dxgi_state;
	/* The GPU list pick_device() drew up, published once choose_present
	 * has decided how the frames go (create_device_objects()): the entry in
	 * use is then the plain one or the one beside it that goes through
	 * DXGI. `pick_via` is what the entry chosen by index said, 1 through
	 * DXGI and 0 not, and -1 where nothing was chosen by index, which
	 * leaves it to choose_present. */
	GpuEntry              pick_list[GPU_LIST_MAX];
	int                   pick_n, pick_cur, pick_via;
	bool                  pick_publish;

	/* The reaper: a thread that does nothing but vkDestroySwapchainKHR, for
	 * the driver whose call takes most of a frame. Started the first time
	 * one is measured slow (reap_async) and not before; see swapchain.c.
	 * reap_lock guards the queue and the two flags after it. What keeps a
	 * destroy and a create apart is process-wide, not here: with two
	 * windows it has to be (swapchain.c's g_swapchain_serial). */
	bool                  reap_async;
	bool                  reap_started;
	FnlThread             reap_thread;
	FnlMutex              reap_lock;
	FnlCond               reap_cond;
	VkSwapchainKHR        reap_q[REAP_MAX];
	int                   reap_n;
	bool                  reap_busy, reap_quit;

	/* The shown waiter: a thread that waits for the frame acking a
	 * configure to reach the screen and then releases the ack
	 * (FnlBackendVtbl::ack_release), so the render thread goes on drawing.
	 * Started at the first such frame; see frame.c. shown_lock guards the
	 * swapchain, the id and the two flags after it. */
	bool                  shown_started;
	FnlThread             shown_thread;
	FnlMutex              shown_lock;
	FnlCond               shown_cond;
	VkSwapchainKHR        shown_swapchain;
	uint64_t              shown_id;
	bool                  shown_busy, shown_quit;

	VkRenderPass          render_pass;
	VkPipelineCache       pipeline_cache;
	/* What the cache file held when create_pipeline_cache() read it, so a
	 * run that compiled nothing new writes nothing back. The window's: two
	 * windows read the one file, and each compares its own blob against what
	 * it read (pipeline.c). */
	size_t                cache_loaded_size;
	uint64_t              cache_loaded_hash;
	VkCommandPool         cmd_pool;

	/* ---- the scene ---------------------------------------------------
	 * A pipeline belongs to the render pass it was built against, and to its
	 * sample count, so a scene pipeline is built once per FnlSceneTarget;
	 * see DESIGN *A pipeline per target*. The scene image carries its own
	 * depth, sized with it. */
	VkRenderPass          scene_pass;      /* into the scene image, 1 sample */
	/* The same, its depth never stored: the pass a single-sampled frame
	 * records into when no mode reads the depth (MLAA, a pinned size).
	 * Identical formats, samples and dependencies, so the caller's
	 * FNL_TARGET_OFFSCREEN pipelines fit both. */
	VkRenderPass          scene_pass_lazy;
	VkRenderPass          scene_pass_ms;   /* multisampled, resolving to it  */
	VkRenderPass          post_pass;       /* colour only: MLAA's output     */
	VkPipelineLayout      blit_layout;     /* fetch: one sampler, no push    */
	VkPipeline            blit_pipeline;
	/* The filter, built twice because a pipeline belongs to one pass: into
	 * its own image when the scene is then scaled, and straight into the
	 * window's pass when the scene is already the size it lands at. */
	VkPipeline            mlaa_pipeline;
	VkPipeline            mlaa_win_pipeline;
	VkDescriptorSetLayout blit_set_layout;
	VkDescriptorPool      blit_pool;
	/* Two sets, allocated with the device and pointed at an image when that
	 * image is built: one reading the scene and one reading the filter's
	 * output. Picking a set at record time is what makes MLAA a toggle
	 * rather than a descriptor update against command buffers that may
	 * still be executing. */
	VkDescriptorSet       blit_set_scene;
	VkDescriptorSet       blit_set_mlaa;
	/* LINEAR, and that one enum is the whole filter the render scale is
	 * resolved with (see shaders/blit.frag). */
	VkSampler             scene_sampler;
	/* What the scene is rendered into at one resolution. The first pair is
	 * the target and exists whenever scene_extent is non-zero. */
	TargetImage           scene;
	TargetImage           scene_depth;
	/* Whether that image was created resident rather than transient -- the
	 * one mode that reads it asks for it, and until it does the depth is a
	 * transient attachment that costs no memory. */
	bool                  scene_depth_resident;
	VkFramebuffer         scene_fb;
	/* The multisampled colour and depth FSAA renders into, and the
	 * framebuffer that resolves the first into the scene image. Built the
	 * first time a frame wants FSAA and kept until the target goes;
	 * `scene_ms_refused` stops a device that would not allocate it being
	 * asked again every frame. */
	TargetImage           scene_ms;
	TargetImage           scene_ms_depth;
	VkFramebuffer         scene_ms_fb;
	bool                  scene_ms_refused;
	/* MLAA's output, the same size and format as the scene image, on the
	 * same terms: built when a frame first filters a scene it then has to
	 * scale. */
	TargetImage           mlaa;
	VkFramebuffer         mlaa_fb;
	bool                  mlaa_refused;
	/* TAAU's resolve (FNL_AA_TAAU): what the pass writes, at the scene's own
	 * resolution and the same format, so the fetch then reads it as it reads
	 * the filter's -- and the image it mixes in, which is the resolved frame
	 * from before, per frame slot so that a frame of one slot never touches
	 * a history the other is writing (each slot's own fence is what orders
	 * its uses). frame_record() re-records rather than reusing a command
	 * buffer in another slot, which is what keeps a set's history and the
	 * slot it is submitted in the same one. */
	TargetImage taau;
	VkFramebuffer taau_fb;
	TargetImage taau_hist[MAX_FRAMES_IN_FLIGHT];
	bool taau_hist_ok[MAX_FRAMES_IN_FLIGHT];
	bool taau_refused;
	/* App::presents when a frame last recorded with TAAU: what
	 * app_taau_follows() reads to say whether this frame's history is the one
	 * before it. */
	uint64_t taau_present;
	/* The resolve's own pipeline, layout and set layout -- two samplers and a
	 * push block, which the fetch's layout has room for neither of -- and the
	 * sets: one per frame slot reading the scene and that slot's history,
	 * plus the fetch's set that reads the resolve. */
	VkPipeline taau_pipeline;
	VkPipelineLayout taau_layout;
	VkDescriptorSetLayout taau_set_layout;
	VkDescriptorSet taau_set[MAX_FRAMES_IN_FLIGHT];
	VkDescriptorSet blit_set_taau;

	/* The snapshot a move takes (App::snap_owed): whether the window's
	 * images may be copied from -- TRANSFER_SRC among the surface's usage,
	 * or the renderer's own images -- and the host buffer the frame copies
	 * itself into where the copy path's readback is not there to serve,
	 * sized for the largest extent yet. And the cover drawn from App::snap
	 * on the device built after a move: its image, the set the fetch's
	 * shaders read it through, the pipeline that blends it by a constant,
	 * the App::snap_gen it was uploaded from, or last tried for, and
	 * whether that try gave the image: one kept from an earlier snapshot,
	 * where a drain refused its release, is never drawn. */
	bool                  snap_usage;
	VkBuffer              snap_buf[1];
	VkDeviceMemory        snap_mem;
	void                 *snap_map[1];
	VkDeviceSize          snap_bytes;
	TargetImage           cover;
	VkDescriptorSet       blit_set_cover;
	VkPipeline            cover_pipeline;
	uint32_t              cover_gen;
	bool                  cover_live;

	/* The text pipeline. Only the atlas for the scale in use is uploaded;
	 * App::font_si is which of the baked scales that is. */
	VkPipelineLayout      text_layout;
	VkPipeline            text_pipeline;
	/* Device offers dualSrcBlend, so the text pipeline can weight the blend
	 * per colour channel and the subpixel variant of text.frag is usable. */
	bool                  dual_src_blend;
	/* VK_KHR_present_id and VK_KHR_present_wait, both, enabled where the
	 * device has them: a present carries `present_id`, and the frame that
	 * acks a configure the window server must see after it waits for it to
	 * be shown (FnlBackendVtbl::ack_when_shown). Fetched by name from the
	 * device, an extension's entry that vk_fns.h does not carry. */
	bool                  present_wait;
	uint64_t              present_id;
	PFN_vkWaitForPresentKHR wait_for_present;
	/* VK_EXT_present_timing, where the bridge asks for the driver's
	 * stamps (VkBridge::driver_stamps) and the device has the extension
	 * and those it depends on: `present_timing` the device's, `timing_on`
	 * the swapchain's -- made with it, on a surface that reports the first
	 * pixel out, with a time domain whose id is timing_domain -- and
	 * timing_owed the presents asked about and not yet answered, which the
	 * swapchain's queue of TIMING_QUEUE bounds. A domain that is the
	 * present stage's own clock (`timing_local`; Mesa's display path has
	 * no other) is put on CLOCK_MONOTONIC by timing_offset_ns, calibrated
	 * at timing_cal_at and again each TIMING_CALIBRATE_S, where the
	 * device reads CLOCK_MONOTONIC beside it (`timing_calibrates`). Each
	 * entry is fetched by name, as present wait's is. */
	bool                  present_timing, timing_on, timing_local;
	bool                  timing_calibrates;
	uint64_t              timing_domain;
	uint32_t              timing_owed;
	int64_t               timing_offset_ns;
	double                timing_cal_at;
	/* Why the display path's frames go unstamped where the bridge asked
	 * for the stamps, "" where they are not; said once, at the first
	 * present the caller counts (timing_said), which is when it matters. */
	char                  timing_why[160];
	bool                  timing_said;
	/* VK_GOOGLE_display_timing in its place, where the device has that
	 * and the bridge knows its clock (VkBridge::driver_clock): each
	 * present carries its id and the past ones are read back each frame
	 * (frame.c, google_take()). */
	bool                  google_timing;
	PFN_vkGetPastPresentationTimingGOOGLE google_past;
#ifdef VK_EXT_present_timing
	PFN_vkSetSwapchainPresentTimingQueueSizeEXT timing_queue_size;
	PFN_vkGetSwapchainTimeDomainPropertiesEXT   timing_domains;
	PFN_vkGetPastPresentationTimingEXT          timing_past;
	PFN_vkGetCalibratedTimestampsKHR            calibrated;
#endif
	/* VK_KHR_get_physical_device_properties2's feature query, where the
	 * instance has the extension; how present wait is asked for on a 1.0
	 * instance. */
	PFN_vkGetPhysicalDeviceFeatures2KHR get_features2;
	/* VK_KHR_get_surface_capabilities2, where the instance enabled it:
	 * present scaling's query, and present timing's. Fetched by name,
	 * as every optional instance entry is. */
	PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR surface_caps2;
	/* The same extension's memory query, and whether the device has
	 * VK_EXT_memory_budget for it to read: the metrics panel's GPU memory
	 * (vk_gpu_memory()). A physical device's query, so the extension is
	 * asked of the device and not enabled on it. */
	PFN_vkGetPhysicalDeviceMemoryProperties2KHR get_memory2;
	bool                  memory_budget;
	VkImage               font_image;
	VkDeviceMemory        font_mem;
	VkImageView           font_view;
	VkSampler             font_sampler;
	VkDescriptorSetLayout text_set_layout;
	VkDescriptorPool      text_pool;
	VkDescriptorSet       text_set;
	/* Per-image glyph buffers, host-visible and permanently mapped; text_map
	 * is where recording writes into them. */
	VkBuffer              text_vb[OVERLAY_MAX_IMAGES];
	VkDeviceMemory        text_vb_mem;   /* one block behind every buffer */

	/* The rectangle and stroke pipelines. Neither reads a descriptor and
	 * both take the same push constant, so they share one layout; their
	 * instances share one buffer per image (see OVERLAY_SHAPE_OFF). */
	VkPipelineLayout      overlay_layout;
	VkPipeline            rect_pipeline;
	VkPipeline            shape_pipeline;
	VkBuffer              ovl_vb[OVERLAY_MAX_IMAGES];
	VkDeviceMemory        ovl_vb_mem;    /* one block behind every buffer */

	/* The image pipeline (fnl_overlay_image()). Its own layout, because its
	 * push block carries the destination rectangle as well as the extent;
	 * the set layout and the sampler are one each, and every image's own
	 * image, memory, view and set live behind FnlImage::gpu, freed by
	 * vk_image_free() (and, with the device, by vk_images_device_gone()).
	 * The pool is for the sets alone, so it is small and freeable. */
	VkPipelineLayout      image_layout;
	VkPipeline            image_pipeline;
	VkDescriptorSetLayout image_set_layout;
	VkDescriptorPool      image_pool;
	VkSampler             image_sampler;
	/* Caller images destroyed while a recording was open, whose draws that
	 * command buffer still names: a list through VkImageObj::retire_next,
	 * let go by vk_images_retire() after the frame is submitted. overlay.c
	 * owns the objects; this is the head. */
	void                 *img_retire;

	/* Where the scene is being recorded, while on_record runs, and null
	 * outside it. Stashed because the public callback signature carries no
	 * graphics type; fnl_vulkan_scene_cmd() reads it back. With it, the
	 * rectangle the viewport and scissor were set to, for
	 * fnl_vulkan_scene_rect(); read only while scene_cb is set. */
	VkCommandBuffer       scene_cb;
	VkRect2D              scene_rect;
	/* What the overlay is being recorded into, set by the renderer around its
	 * own drawing. */
	VkCommandBuffer       overlay_cb;
	/* OverlayPush is live in overlay_cb. Cleared where that stops being so:
	 * a new recording, and a bind of a pipeline whose layout is not
	 * compatible for push constants, which leaves the values undefined. */
	bool                  overlay_pushed;

	/* ---- frame sync -------------------------------------------------- */
	VkSemaphore           img_available[MAX_FRAMES_IN_FLIGHT];
	VkFence               in_flight[MAX_FRAMES_IN_FLIGHT];

	/* The swapchain's format, and the format of the colour targets the
	 * renderer draws into: the same as the swapchain's, or
	 * R16G16B16A16_SFLOAT under HDR, where the window's pass and the overlay
	 * write an image of this format (present[]) and a last pass encodes it
	 * into the 10-bit PQ swapchain. */
	VkSurfaceFormatKHR    surf_format;
	VkFormat              color_format;
	/* The swapchain presents PQ, and nothing blends into it: render_pass is
	 * the window's pass over present[] (color_format, sampled), and
	 * encode_pass reads that into the swapchain's own. */
	bool                  hdr;
	VkRenderPass          encode_pass;
	VkPipeline            encode_pipeline;
	/* Under HDR, per swapchain image: the window's linear image, the
	 * framebuffers the encode pass draws through, and the descriptor sets it
	 * reads each image through. */
	TargetImage           present[OVERLAY_MAX_IMAGES];
	VkFramebuffer         encode_fb[OVERLAY_MAX_IMAGES];
	VkDescriptorSet       encode_set[OVERLAY_MAX_IMAGES];
	/* Whether those sets have been pointed at an image yet: rewriting one a
	 * command buffer still pending names is what the rebuild's drain is
	 * for. */
	bool                  encode_written;
	/* What the vsync toggle switches to, resolved with the surface. Vsync
	 * itself is FIFO, which every surface has. */
	VkPresentModeKHR      mode_tearing;    /* MAILBOX, else IMMEDIATE */
};

#endif /* FNL_VK_STATE_H */
