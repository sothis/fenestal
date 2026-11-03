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
 * in. */
#define TESTBED_RGB(r, g, b) ((FnlColor){ (r), (g), (b), 1.0f })

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
	char     name[256];
	char     info[256];
	char     api[160];
	unsigned vendor_id, device_id;
} TestbedDriver;
bool scene_vulkan_driver(FnlApp *app, TestbedDriver *d);
bool scene_gl_driver(FnlApp *app, TestbedDriver *d);
/* fnl_gl_version() where OpenGL draws, 0 elsewhere: the one call into
 * fnl_gl.h the renderer-neutral files make, through scene_gl.c, which a
 * build without OpenGL does not have. */
int  scene_gl_version(FnlApp *app);

/* The renderer as the title and the status line name it: its name, the
 * API it goes through where it presents through another ("Vulkan via
 * D3D11"), and OpenGL's version where OpenGL draws ("OpenGL 4.1"), which
 * is 4.1 on a device with no 4.5. main.c. */
void testbed_renderer_label(FnlApp *app, char *out, size_t size);
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
/* The GPU memory this process holds on the GPU drawing, in MiB: the
 * scene's API's figure, or the system's for OpenGL. From on_overlay. */
bool testbed_vram_mib(FnlApp *app, double *mib);

/* ---- titlebar_custom.c: the bar drawn with --custom-titlebar --------------
 *
 * Exclusive with the library's own: with this one on,
 * fnl_titlebar_reserve() refuses and the library's controls draw nothing. */
void testbed_titlebar_custom_enable(void);
bool testbed_titlebar_custom_enabled(void);
int  testbed_titlebar_custom_height(void);
int  testbed_titlebar_custom_min_width(void);
void testbed_titlebar_custom_event(FnlApp *app, const FnlEvent *ev);
void testbed_titlebar_custom_draw(FnlApp *app, const FnlFrame *frame);

#endif /* TESTBED_H */
