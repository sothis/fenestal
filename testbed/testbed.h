/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the testbed's files share: the scenes' callbacks, the overlay, and the
 * replacement titlebar.
 *
 * Nothing in here is a graphics API's own: the header a program's other files
 * include should not commit them to one, even though each scene file behind
 * it is written entirely in one.
 */

#ifndef TESTBED_H
#define TESTBED_H

#include <fenestal/fenestal.h>

#include <stdbool.h>

/* Authored in sRGB, which is what FnlColor takes -- the numbers a colour
 * picker gives. The library converts to whatever space the surface blends
 * in. A function and not a compound literal: cl.exe has no such literal,
 * and this is the one macro that would otherwise be one everywhere. */
static inline FnlColor testbed_rgb(float r, float g, float b)
{
	const FnlColor c = { r, g, b, 1.0f };
	return c;
}
#define TESTBED_RGB(r, g, b) testbed_rgb((r), (g), (b))

/* What F4 puts on the clipboard. One word, so that a harness reading it back
 * out of an echoed line can find where it ends. */
#define TESTBED_CLIPBOARD_TEXT "fenestal-testbed-copied-this"

/* ---- scene_<api>.c: the test pattern, once per graphics API -------------
 *
 * Build and drop the pipelines, on the render thread: once at startup and
 * again around every device loss. Then record the scene into whatever the
 * library hands over. The build compiles a file for each renderer the library
 * has, and main.c calls the one fnl_renderer() names. */
void scene_vulkan_on_gpu_created(void *ud, FnlApp *app);
void scene_vulkan_on_gpu_lost(void *ud, FnlApp *app);
void scene_vulkan_on_record(void *ud, FnlApp *app, const FnlFrame *frame);

void scene_gl_on_gpu_created(void *ud, FnlApp *app);
void scene_gl_on_gpu_lost(void *ud, FnlApp *app);
void scene_gl_on_record(void *ud, FnlApp *app, const FnlFrame *frame);

void scene_d3d12_on_gpu_created(void *ud, FnlApp *app);
void scene_d3d12_on_gpu_lost(void *ud, FnlApp *app);
void scene_d3d12_on_record(void *ud, FnlApp *app, const FnlFrame *frame);

void scene_metal_on_gpu_created(void *ud, FnlApp *app);
void scene_metal_on_gpu_lost(void *ud, FnlApp *app);
void scene_metal_on_record(void *ud, FnlApp *app, const FnlFrame *frame);

/* The GPU memory this process holds on the GPU drawing, in MiB, as its API
 * says it: Vulkan's memory budget, DXGI's QueryVideoMemoryInfo, Metal's
 * currentAllocatedSize. False where it says nothing. From on_overlay, on
 * the render thread. OpenGL keeps no figure of one process's; gpumem.c
 * asks the system for it instead. */
bool scene_vulkan_vram(FnlApp *app, double *mib);

/* The driver drawing, as the scene's API names it, for the benchmark's
 * record: its name and version text, the API's version, and the PCI ids;
 * "" and 0 for what it does not say. From on_overlay, on the render
 * thread, where OpenGL's context is current. */
typedef struct {
	char name[256];
	char info[256];
	char api[160];
	unsigned vendor_id, device_id;
} TestbedDriver;
bool scene_vulkan_driver(FnlApp *app, TestbedDriver *d);
bool scene_gl_driver(FnlApp *app, TestbedDriver *d);
/* fnl_gl_version() where OpenGL draws, 0 elsewhere: the one call into
 * fnl_gl.h the renderer-neutral files make, through scene_gl.c, which a
 * build without OpenGL does not have. */
int scene_gl_version(FnlApp *app);

/* The renderer as the title and the status line name it: its name, the
 * API it goes through where it presents through another ("Vulkan via
 * D3D11"), and OpenGL's version where OpenGL draws ("OpenGL 4.1"), which
 * is 4.1 on a device with no 4.5. main.c. */
void testbed_renderer_label(FnlApp *app, char *out, size_t size);
/* The clock the scene's shaders are given: the frame's own scene_time, or,
 * under --freeze, a held value so the picture keeps one pose while the
 * library clock (and so the temporal jitter) runs. */
float testbed_scene_time(const FnlFrame *frame);

/* ---- how a window's scene is shown ---------------------------------------
 *
 * Every window is a window on the same host and the same scene shader, so
 * what it draws of it is the program's to say. Three kinds, which main.c's F12
 * key cycles for the second window (F10 shows and hides that window, and its
 * close button hides it too):
 *
 *   OWN     a picture of its own. The second window draws the scene shader's
 *           other scene -- the turning cube other_scene() renders -- so the
 *           two windows show different content and not one picture twice;
 *           where the window's own scene is the first's, that is the pattern
 *           laid out on its own canvas.
 *   JOIN    one picture across the windows: the pattern laid out on a canvas
 *           as wide as the windows side by side, each window showing the
 *           part of it at its own offset. The star and the sweep cross the
 *           seam rather than repeating.
 *   MIRROR  the whole pattern, the same in every window -- what two windows
 *           of one host drew before this was a choice.
 *
 * The two windows need not agree: the first is a mirror while the second
 * joins it, and neither draws a part of the other's pattern that it is not
 * told to. What a window draws is its own view, set by whoever holds that
 * window's app. */
typedef enum {
	TESTBED_VIEW_MIRROR = 0,
	TESTBED_VIEW_JOIN   = 1,
	TESTBED_VIEW_OWN    = 2,
} TestbedView;

/* The view one window draws: which kind, in window points (the size a window
 * is, in the units the overlay speaks) and in the canvas's pixels. Both a
 * scene_*.c and the status readout ask it, so what the readout says is what
 * the scene drew. From on_record or on_overlay, on the render thread, for
 * that window's app; a pure function of the app's place on the host and the
 * size, so two windows cannot disagree about a shared canvas. */
typedef struct {
	TestbedView kind;
	uint32_t    content;   /* 0: the pattern; 1: the other scene */
	float       size[2];   /* this window's scene, px */
	float       canvas[2]; /* the pattern's whole canvas, px */
	float       origin[2]; /* this window's corner inside it, px */
	float       tint[4];   /* linear-light multiplier, 1,1,1 for a mirror */
	float       time_bias; /* this window's phase on the scene's clock */
} TestbedViewSpec;

void testbed_scene_view(FnlApp *app, const FnlFrame *frame,
						TestbedViewSpec *out);
/* The current kind's name -- "mirror", "join" or "own" -- for the status
 * readout and the harness's log line. */
const char *testbed_view_name(void);

/* Forget a window's picture when its window is given back. The table is keyed
 * by the app, and a window closed and opened again can be handed the same
 * address: its slot would then hold an image made on the destroyed device and
 * draw it on the new one. The image itself is the library's to free at
 * fnl_shutdown(); this only forgets it. Called from the main thread once the
 * window's render thread is joined, never from a callback. */
void testbed_overlay_picture_forget(FnlApp *app);
bool scene_d3d12_vram(FnlApp *app, double *mib);
bool scene_metal_vram(FnlApp *app, double *mib);
bool scene_d3d12_driver(FnlApp *app, TestbedDriver *d);
bool scene_metal_driver(FnlApp *app, TestbedDriver *d);

/* ---- gpumem.c: the GPU memory OpenGL cannot say ---------------------------
 *
 * This process's, on the GPU the renderer names `gpu`, in MiB, from the
 * system: NVML for NVIDIA's own driver, the kernel's DRM fdinfo for Mesa's
 * where the driver keeps one (Intel, AMD). False where there is none --
 * nouveau's, and every system but Linux. */
bool testbed_gpu_mem_os(const char *gpu, double *mib);
/* What it opened to answer, let go of: from on_gpu_lost, since NVML holds
 * NVIDIA's driver while it runs and the GPU may be moving to another
 * kernel driver. The next figure asked for opens it again. */
void testbed_gpu_mem_os_release(void);

/* ---- proc.c: this process's CPU and resident memory ------------------------
 *
 * The CPU as the share of one core used since the last sample, at most
 * twice a second, and the resident set in MiB; -1 for what the system
 * will not say, and the CPU's -1 at the first call. */
void testbed_proc_sample(double *cpu_pct, double *rss_mib);
/* The resident set now, in MiB, unthrottled; -1 where it is not known. */
double testbed_proc_rss_mib(void);

/* ---- overlay.c: the keybindings panel, the status readout, the crosshair --
 *
 * Both are called from on_overlay, events first. */
void testbed_overlay_event(FnlApp *app, const FnlEvent *ev);
void testbed_overlay_draw(FnlApp *app, const FnlFrame *frame);
/* --footprint-log: this process's CPU, RAM and VRAM said on stderr twice a
 * second, the figures the library's metrics panel shows. Before
 * fnl_init(). */
void testbed_overlay_footprint_log(void);

/* --frame-cpu-log: the per-frame CPU figure (fnl_frame_cpu_ms()) said on
 * stderr twice a second, and nothing else turned on -- the panel's own
 * figures at rest, where --footprint-log would ask for sampling and count
 * the frames the panel draws there. */
void testbed_overlay_frame_cpu_log(void);

/* --frame-stats-log: the frame loop's totals (fnl_frame_stats()) and the
 * rows they are made of (fnl_frame_samples()) said on stderr twice a
 * second, with the two figures those means are kept from. Turns frame-stats
 * counting on: what a window at rest must hold while the panel's own frames
 * are drawn there. */
void testbed_overlay_frame_stats_log(void);

/* --reserve-log: fnl_titlebar_reserve()'s answer for a caller's run, with
 * the window's width, on stderr twice a second -- where the run sits beside
 * the library's controls, and the width at which the output box gives way. */
void testbed_overlay_reserve_log(void);

/* --shapes: the shapes panel on from the first frame, where O would toggle
 * it. For a round that reads pixels: a keypress is a thing the harness has
 * to arrange, and the panel answers whether the four filled forms draw at
 * all. */
void testbed_overlay_shapes_on(void);
/* The GPU memory this process holds on the GPU drawing, in MiB: the
 * scene's API's figure, or the system's for OpenGL. From on_overlay. */
bool testbed_vram_mib(FnlApp *app, double *mib);

/* ---- titlebar_custom.c: the bar drawn with --custom-titlebar --------------
 *
 * Exclusive with the library's own: with this one on,
 * fnl_titlebar_reserve() refuses and the library's controls draw nothing. */
void testbed_titlebar_custom_enable(void);
bool testbed_titlebar_custom_enabled(void);
int testbed_titlebar_custom_height(void);
int testbed_titlebar_custom_min_width(void);
void testbed_titlebar_custom_event(FnlApp *app, const FnlEvent *ev);
void testbed_titlebar_custom_draw(FnlApp *app, const FnlFrame *frame);

#endif /* TESTBED_H */
