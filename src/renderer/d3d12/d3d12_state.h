/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Everything about the Direct3D 12 renderer that no file outside
 * src/renderer/d3d12/ may see: its state, behind App::renderer_state.
 * d3d12_internal.h includes it, after <d3d12.h>; nothing else does.
 *
 * App::renderer_state is allocated by create_instance() and freed by
 * destroy_instance(), so it exists from the first entry of the seam to the
 * last. The [any] entries -- device_name, present_mode_name, max_samples --
 * read only App's own atomics, and App::gpu_name under App::lock, and never
 * it.
 */

#ifndef FNL_D3D12_STATE_H
#define FNL_D3D12_STATE_H

/* What the state is made of: the D3D12 and DXGI types, the two counters and
 * bool it holds, and MAX_FRAMES_IN_FLIGHT. Included here as well as from
 * d3d12_internal.h, which is the only file that includes this one, so the
 * header stands on its own where it is read -- the guards make the second
 * inclusion of each a no-op. */
#include "core/app.h"

#include <d3d12.h>
#include <dxgi1_6.h>

/* The renderer's state, from an App. */
#define D3D(a) ((a)->renderer_state)

/* Back buffers in the swapchain. Three: a flip-model swapchain wants two
 * at least, and the third keeps the queue from waiting on the display. */
#define D3D_BUFFERS 3

/* The fixed descriptor slots. Render targets: the back buffers, then the
 * scene's three images; depth: the window's, then the scene's two; shader
 * resources, in the one shader-visible heap: the atlas, the scene, MLAA's
 * output, the cover; samplers: nearest for the atlas, linear for the
 * scene. */
enum {
	RTV_BACK0 = 0,
	RTV_SCENE = D3D_BUFFERS,
	RTV_SCENE_MS,
	RTV_MLAA,
	RTV_TAAU,
	RTV_TAAU1, /* one a frame slot: RTV_TAAU + a->frame */
	RTV_COUNT
};
enum { DSV_WINDOW = 0, DSV_SCENE, DSV_SCENE_MS, DSV_COUNT };
/* TAAU's resolve reads two images in one draw, and a descriptor table reads
 * *consecutive* descriptors: so the pairs it hands the table are reserved as
 * pairs -- {scene, the slot's other image} -- rather than copied
 * into place each frame, which would be work in the recording for a view
 * that never changes. The scene's views in those pairs are made again
 * wherever the scene's image is (d3d_scene_set_size()). */
enum {
	SRV_ATLAS = 0,
	SRV_SCENE,
	SRV_MLAA,
	SRV_COVER,
	SRV_TAAU,
	SRV_TAAU1, /* one a frame slot: SRV_TAAU + a->frame */
	SRV_PAIR0,
	SRV_PAIR0_H,
	/* FNL_AA_TAAU's resolve reads three images in one draw -- the scene,
	 * the history, and the scene's depth -- so the depth's view sits right
	 * after each pair, where a three-descriptor table starting at the pair
	 * reaches it. */
	SRV_PAIR0_D,
	SRV_PAIR1,
	SRV_PAIR1_H,
	SRV_PAIR1_D,
	SRV_COUNT
};
/* The resolve's two samplers are told apart by register, and both want the
 * scene's linear filter: the second is an identical sampler, not the
 * atlas's. */
enum { SMP_NEAREST = 0, SMP_LINEAR, SMP_LINEAR2, SMP_LINEAR3, SMP_COUNT };

/* Caller images (fnl_image_create()) take a descriptor each from the srv
 * heap, after the fixed slots above, which name the library's resources and
 * are never handed on. RendererState::image_used is a bit a slot, so this is
 * a word -- the bound Vulkan's descriptor pool has, and for its reason.
 *
 * Two past the last image slot: the one root signature declares a
 * three-descriptor srv range (the TAAU resolve reads three images in one
 * draw), and a descriptor table must lie inside its heap. An image in the
 * last slot starts a table whose other two descriptors are past its view
 * (d3d12/overlay.c, image_srv_gpu); only t0 is read by the image shader, so
 * the range is otherwise wasted, but it has to exist.
 *
 * One heap and not a second one: a list binds at most one heap of each type,
 * and two CBV_SRV_UAV heaps in one SetDescriptorHeaps() make every Close()
 * of the frame's list fail with E_INVALIDARG on a real driver -- vkd3d-proton
 * takes it, which is what hid this in the wine rounds. */
#define IMAGE_SETS_MAX 64
_Static_assert(IMAGE_SETS_MAX <= 64, "image_used is one word of slots");
#define SRV_HEAP_COUNT (SRV_COUNT + IMAGE_SETS_MAX + 2)

/* An image the GPU draws into, with its depth where it has one. Null
 * together when there is none. `state` is the colour image's resource state
 * as the last recording left it, which the next barrier starts from. */
typedef struct {
	ID3D12Resource *color;
	ID3D12Resource *depth;
	D3D12_RESOURCE_STATES state;
} D3dTarget;

struct RendererState {
	/* ---- the factory, and the adapter the device is on ----------------- */
	IDXGIFactory4 *factory;
	IDXGIAdapter1 *adapter;
	/* DXGI_FEATURE_PRESENT_ALLOW_TEARING: an uncapped present that tears,
	 * where vsync is off. Asked of the factory once. */
	bool tearing;
	/* WARP or another software adapter: its present waits for the frame. */
	bool software;

	/* ---- the output's colour --------------------------------------------
	 * The formats every image is made in: the swapchain's buffers, and the
	 * colour views and targets over them. SDR is R8G8B8A8_UNORM drawn
	 * through _SRGB views; HDR is R16G16B16A16_FLOAT, which holds linear
	 * light as it is written, presented as scRGB. Chosen once per device
	 * (d3d_bridge_swapchain()), before any target or pipeline exists. */
	DXGI_FORMAT back_format, color_format;
	/* The output presents linear light above SDR white: the space was asked
	 * for, the display is in an advanced colour mode, and the swapchain
	 * takes scRGB. False through the Direct3D 11 swapchain, whose shared
	 * textures are 8-bit. */
	bool hdr;
	/* The value that is SDR white in the scRGB target's unit (sdr white
	 * nits / 80), and the output's peak as a multiple of SDR white -- what
	 * FnlOutputColor::peak takes, not nits (a caller scaling it by 80
	 * alone is right only where sdr_scale is 1). */
	float sdr_scale, peak;

	/* ---- the device ---------------------------------------------------- */
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
	/* A Debug tree's: what the debug layer said, drained into the log once
	 * a frame. NULL where the layer is not installed. */
	ID3D12InfoQueue *info;

	/* One fence for the queue. `fenced` is the last value signalled, and
	 * slot_value[] what each frame slot's last submission signalled, which
	 * the slot waits for before its allocator and arenas are reused. */
	ID3D12Fence *fence;
	HANDLE fence_event;
	uint64_t fenced;
	uint64_t slot_value[MAX_FRAMES_IN_FLIGHT];

	ID3D12CommandAllocator *alloc[MAX_FRAMES_IN_FLIGHT];
	/* Two timestamps per slot, at the first and the last command of its
	 * list, resolved into ts_read at the list's end, for
	 * fnl_frame_gpu_ms(); ts_freq the queue's ticks a second, ts_unread
	 * whether a slot's pair has been written since it was read, and
	 * ts_index the frame that wrote it (app_gpu_timed()). NULL where the
	 * heap would not be made. */
	ID3D12QueryHeap *ts_heap;
	ID3D12Resource *ts_read;
	UINT64 ts_freq;
	bool ts_unread[MAX_FRAMES_IN_FLIGHT];
	uint64_t ts_index[MAX_FRAMES_IN_FLIGHT];
	ID3D12GraphicsCommandList *list;
	/* Whether `list` is open for recording. */
	bool recording;

	/* ---- the swapchain -------------------------------------------------- */
	IDXGISwapChain3 *swapchain;
	/* The frame-latency object: signalled when the swapchain will take
	 * another frame, which is what the frame waits on before it is drawn;
	 * and how many frames it lets be queued, 1 at rest and 2 while the
	 * window is being sized (frame.c). */
	HANDLE frame_wait;
	UINT latency;
	/* The frame-latency object was waited on and no present has released
	 * it since: a frame that bailed before its present. The next frame
	 * does not wait again, which would sit out the whole timeout. */
	bool frame_waited;
	/* Waits skipped -- on a frame that answered a size, and every frame
	 * under sync interval 0 (frame.c) -- each a count the object may be
	 * ahead by; taken back without blocking at the next wait. */
	int frame_owed;
	UINT sc_flags;
	/* The swapchain made from a Direct3D 11 device, the back buffers
	 * shared with it (present_d3d11.c); NULL for this device's own.
	 * `via11_on` is which the next swapchain is made from; the window is
	 * measured at rest (frame.c, via11_measure()) until `via11_settled`,
	 * a frame late on `late` of `seen` presents, and `via11_switch` asks
	 * the next frame to make the swapchain again the other way. A
	 * device's own, for each device made (create_device_objects()), and
	 * measured again at each size (App::size_at, as `via11_at` was when
	 * it settled) unless FNL_D3D12_DXGI11 decided it (`via11_forced`). */
	struct D3dVia11 *via11;
	bool via11_on, via11_settled, via11_switch, via11_forced;
	int via11_seen, via11_late;
	double via11_at;
	ID3D12Resource *back[D3D_BUFFERS];
	/* The buffers' size: App::extent, or larger, the window showing their
	 * corner (d3d_window_images()). */
	int buf_w, buf_h;
	/* The window's depth, at the buffers' size, for a scene drawn straight
	 * in. */
	ID3D12Resource *window_depth;
	/* The present interval the swapchain presents with: 1 or 0, mirrored
	 * into App::present_mode. */
	int interval;
	/* The last present said the window is covered (DXGI_STATUS_OCCLUDED). */
	bool occluded;
	/* The frame that answers a size waits to be shown (frame.c,
	 * wait_shown()): set when the swapchain gives no frame statistics, or
	 * none in DISJOINT_GIVE_UP waits in a row, after which this swapchain
	 * is not asked again; cleared with a new one. */
	bool no_stats;
	int stats_unseen;
	/* Under wine, where IDXGIOutput::WaitForVBlank waits for no vertical
	 * blank: the wait polls the statistics instead (frame.c). */
	bool blank_estimated;
	/* The shown waiter, which makes that wait on a thread of its own and
	 * releases the ack (FnlBackendVtbl::ack_release), so the render thread
	 * goes on drawing. Started at the first frame that acks, its lock
	 * made then whether or not the thread could be (shown_ready);
	 * shown_lock guards the count, the output, the two flags after them,
	 * and no_stats and stats_unseen above. */
	bool shown_ready, shown_started;
	FnlThread shown_thread;
	FnlMutex shown_lock;
	FnlCond shown_cond;
	UINT shown_want;
	IDXGIOutput *shown_out;
	bool shown_busy, shown_quit;

	/* ---- descriptors ---------------------------------------------------- */
	ID3D12DescriptorHeap *rtv_heap;
	ID3D12DescriptorHeap *dsv_heap;
	ID3D12DescriptorHeap *srv_heap;
	ID3D12DescriptorHeap *smp_heap;
	UINT rtv_inc, dsv_inc, srv_inc, smp_inc;

	/* ---- the scene's targets ----------------------------------------------
	 * As the other renderers': the scene at one resolution, sampled by the
	 * fetch; the multisampled pair FSAA draws into and resolves from; and
	 * MLAA's output. A refusal is remembered. */
	D3dTarget scene;
	D3dTarget scene_ms;
	bool scene_ms_refused;
	D3dTarget mlaa;
	bool mlaa_refused;
	/* TAAU's resolve: one image a frame slot, each with an RTV and an SRV. A
	 * frame's pass writes its slot's and mixes in the other's, the frame
	 * before's; the fetch reads the one written. No copy: `taau_hist_ok[s]`
	 * is whether slot s's image holds a frame that was recorded. */
	D3dTarget taau_hist[MAX_FRAMES_IN_FLIGHT];
	bool taau_hist_ok[MAX_FRAMES_IN_FLIGHT];
	bool taau_refused;
	uint64_t taau_present;
	int samples; /* what FSAA multisamples at */

	/* What on_record draws into, while it runs; null otherwise.
	 * fnl_d3d12_scene() reads it back. */
	ID3D12GraphicsCommandList *scene_list_now;
	D3D12_CPU_DESCRIPTOR_HANDLE scene_rtv_now;
	D3D12_CPU_DESCRIPTOR_HANDLE scene_dsv_now;
	D3D12_RECT scene_rect_now;

	/* ---- the overlay ---------------------------------------------------- */
	ID3D12RootSignature *root;
	ID3D12PipelineState *pso_rect;
	ID3D12PipelineState *pso_shape;
	ID3D12PipelineState *pso_text;
	ID3D12PipelineState *pso_blit;
	ID3D12PipelineState *pso_mlaa;
	/* The resolve: the fetch's vertex stage and a fragment stage reading two
	 * images, so a state of its own. */
	ID3D12PipelineState *pso_taau;
	/* The cover: the fetch's shaders blended by a constant, drawing
	 * App::snap -- the frame read back before a move -- as a texture of
	 * this device, the App::snap_gen it was uploaded from or last tried
	 * for (d3d_overlay_cover()), and whether that try gave the texture:
	 * one kept from an earlier snapshot, where a drain refused its
	 * release, is never drawn. And the readback buffer a frame
	 * copies its back buffer into for that snapshot (frame.c), sized for
	 * the largest extent yet, rows on the pitch alignment. */
	ID3D12PipelineState *pso_cover;
	ID3D12Resource *cover_tex;
	uint32_t cover_gen;
	bool cover_live;
	ID3D12Resource *snap_read;
	size_t snap_read_bytes;
	size_t snap_pitch;

	/* Per frame slot: the overlay's arenas, upload-heap buffers mapped for
	 * good (App::text_map and App::ovl_map point into them). */
	ID3D12Resource *text_buf[MAX_FRAMES_IN_FLIGHT];
	ID3D12Resource *ovl_buf[MAX_FRAMES_IN_FLIGHT];

	ID3D12Resource *font_tex;

	/* The caller's images (fnl_image_create()): one pipeline state, and the
	 * slots of the srv heap their views take (SRV_COUNT on), taken and given
	 * as the images are made and freed (d3d_overlay_image(),
	 * d3d_image_free()). `image_used` has a bit a slot. */
	ID3D12PipelineState *pso_image;
	uint64_t image_used;
	/* Caller images destroyed while a recording was open, whose draws that
	 * list still names: a list through D3dImageObj::retire_next, let go by
	 * d3d_images_retire() after the frame is submitted. overlay.c owns the
	 * objects; this is the head. */
	void *img_retire;
	/* The uploads submitted and not yet known done: D3dUpload, overlay.c's;
	 * let go by d3d_images_retire(). */
	void *upl_keep;

	/* Whether the overlay is being drawn: a flush outside it draws
	 * nothing, as one outside a Vulkan recording does. */
	bool overlay_on;
};

#endif /* FNL_D3D12_STATE_H */
