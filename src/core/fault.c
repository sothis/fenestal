/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Fault injection: the recovery and fallback paths that never run on a
 * machine that works, made to run by name. DESIGN *Forcing the paths that
 * never run* is the table of names and what each should produce.
 *
 *   FNL_FAULT=device-lost%300                every 300th frame
 *   FNL_FAULT=create-surface*3,device-lost@1 the first three, then once
 *
 * A spec is a name and how often: bare is every time it is reached, @N the
 * Nth time only, %N every Nth, *N the first N. Read by the process's first
 * fnl_init(), and counted across every lifetime after it.
 *
 * Debug trees only. Under NDEBUG FNL_FAULT() is the constant false and none
 * of this is compiled, so a release build has no fault path a variable in
 * its environment could switch on.
 */

#include "core/app.h"

#ifndef NDEBUG

/* Every site there is. A name in FNL_FAULT that is not here is reported at
 * init, so a misspelt fault is not a quiet pass. */
static const char *const SITES[] = {
	"no-dual-src",          /* create_device: no dualSrcBlend */
	"no-present-timing",    /* create_device: no driver present stamps */
	"no-msaa",              /* every renderer: one sample is the best */
	"unorm-surface",        /* query_surface_info: a UNORM format chosen */
	"no-viewporter",        /* wayland: wp_viewporter not bound */
	"wl-accelerated-relative", /* relative_motion: the accelerated delta */
	"backend-no-display",   /* backend_choose_and_open: no server here */
	"renderer-refused",     /* renderer_choose_and_build: none here */
	"renderer-move-refused", /* renderer_swap: the renderer moved to */
	"gpu-driver-refused",   /* app_gpu_driver_make: the GPU stays put */
	"gpu-driver-missed",    /* app_gpu_driver_landed: drawn elsewhere */
	"gpu-driver-uncrossed", /* renderer_cross: the frames pause instead */
	"gpu-driver-leftover",  /* gpu_driver_leftover: no move, none again */
	"device-lost",          /* draw_frame: the device is lost */
	"surface-lost",         /* draw_frame: the surface is lost */
	"create-surface",       /* recover_from_loss: create_surface refuses */
	"recover-objects",      /* recover_from_loss: the rebuild fails */
	"framebuffers",         /* create_framebuffers fails */
	"scene-target",         /* create_scene_target: no offscreen target */
	"scene-ms",             /* create_scene_ms: no multisampled pair */
	"scene-mlaa",           /* create_scene_mlaa: no MLAA image */
	"x11-refresh",          /* x11_refresh_read: no refresh rate known */
	"x11-sync-at-swap",     /* x11_flush: the sync value not kept for Present */
	"x11-clip-unstamped",   /* clip_stamp: copy and paste at CurrentTime */
	"kms-no-keymap",        /* keymap_load: the files give no keymap */
	"kms-no-cursor",        /* glyph_make: no buffer for the cursor plane */
	"kms-no-display",       /* acquire_display: the copy path instead */
	"kms-no-headless",      /* copy_surface: images of the renderer's own */
	"kms-mode-refused",     /* a mode switched to will not show */
	"kms-gl-copy",          /* scan_begin: OpenGL copies onto the output */
	"kms-swapchain-refused", /* create_swapchain: a display mode's refused */
	"atlas-out-of-vram",    /* build_atlas: the atlas in system memory */
	"stale-swapchain",      /* swap_stale_note: the window's size nudged */
	"gl-opaque-refused",    /* gl_bridge_resize: no opaque EGL surface */
	"gl-41",                /* gl_load: a 4.5 context drawn as 4.1 */
	"d3d12-no-scaling-none", /* d3d_bridge_swapchain: stretch instead */
	"occluded",             /* the frame loop: the renderer says covered */
	"win32-no-raw-wheel",   /* raw_wheel_open: the wheel by message alone */
	"win32-no-raw-thread",  /* win32_open: input stamped by the tick alone */
	"win32-no-imm32",       /* imm_load: the system shows the composition */
	"d3d12-composed",       /* via11_measure: DWM composes the window */
	"d3d11-present-refused", /* d3d_via11_open: this device's own again */
	"d3d12-after-swapchain", /* create_device_objects: fails after it */
	"metal-after-layer",    /* create_device_objects: fails after it */
};
#define SITE_COUNT (sizeof SITES / sizeof SITES[0])

typedef enum { EVERY, NTH, PERIOD, FIRST } FaultMode;

typedef struct {
	bool         armed;
	FaultMode    mode;
	uint32_t     n;
	FnlAtomicU32 hits;
} Fault;

/* By site index. Written by fault_init() on the main thread before any
 * other thread exists, and only the counters move afterwards. */
static Fault faults[SITE_COUNT];

static int site_index(const char *name, size_t len)
{
	for (size_t i = 0; i < SITE_COUNT; i++)
		if (strlen(SITES[i]) == len && memcmp(SITES[i], name, len) == 0)
			return (int)i;
	return -1;
}

/* ---- FNL_SELFTEST: one deliberate bug, for the checker that should see it
 *
 * Silence from a checker is worth something only if it was on (DESIGN
 * *Checking it*), so each tree can be made to commit the one bug its
 * checker exists for, and tools/drive/selftest.py asks it to:
 *
 *   asan    a read one byte past a heap block
 *   ubsan   a signed overflow
 *   tsan    two threads writing one int with nothing between them
 *   assert  an FNL_ASSERT that fails
 *   gl      an invalid GL call, for a debug context's output to report;
 *           committed by the OpenGL renderer once that context exists
 *           (src/renderer/gl/device.c), since nothing here may name GL
 *
 * Everything the compiler could see through is volatile, or the bug would
 * be folded away -- or reported at compile time, which is not the question.
 */

static volatile int race_target;

static void *race_writer(void *arg)
{
	(void)arg;
	for (int i = 0; i < 1000; i++)
		race_target = race_target + 1;
	return NULL;
}

static void selftest(void)
{
	const char *what = getenv("FNL_SELFTEST");
	if (!what || !*what)
		return;
	fnl_log(FNL_LOG_WARN, "FNL_SELFTEST: committing '%s'", what);

	if (!strcmp(what, "asan")) {
		volatile size_t past = 8;
		char *p = malloc(8);
		if (p) {
			volatile char c = p[past];
			(void)c;
			free(p);
		}
	} else if (!strcmp(what, "ubsan")) {
		volatile int big = 0x7fffffff;
		volatile int sum = big + 1;
		(void)sum;
	} else if (!strcmp(what, "tsan")) {
		FnlThread t;
		if (fnl_thread_start(&t, race_writer, NULL)) {
			race_writer(NULL);
			fnl_thread_join(t);
		}
	} else if (!strcmp(what, "assert")) {
		volatile bool held = false;
		FNL_ASSERT(held);
	} else if (!strcmp(what, "gl")) {
		/* The OpenGL renderer's, through FNL_SELFTEST("gl"). */
	} else {
		fnl_log(FNL_LOG_WARN, "FNL_SELFTEST: no such test '%s'", what);
	}
}

bool fault_selftest(const char *name)
{
	const char *what = getenv("FNL_SELFTEST");
	return what && !strcmp(what, name);
}

/* Once per process, as the table is: a count such as @3 runs across every
 * lifetime the process has, which is what lets a fault land in the third. */
void fault_init(void)
{
	static bool parsed;
	if (parsed)
		return;
	parsed = true;
	selftest();

	const char *spec = getenv("FNL_FAULT");
	if (!spec || !*spec)
		return;

	while (*spec) {
		const char *end = strchr(spec, ',');
		const size_t len = end ? (size_t)(end - spec) : strlen(spec);
		size_t name_len = strcspn(spec, "@%*,");
		if (name_len > len)
			name_len = len;

		const int i = site_index(spec, name_len);
		if (i < 0) {
			fnl_log(FNL_LOG_WARN, "FNL_FAULT: no fault site '%.*s'",
					(int)name_len, spec);
		} else {
			Fault *f = &faults[i];
			f->armed = true;
			f->mode  = EVERY;
			f->n     = 1;
			if (name_len < len) {
				const char op = spec[name_len];
				const long n = strtol(spec + name_len + 1, NULL, 10);
				f->n    = n > 0 && n <= 1000000 ? (uint32_t)n : 1u;
				f->mode = op == '@' ? NTH : op == '%' ? PERIOD : FIRST;
			}
			fnl_log(FNL_LOG_WARN, "FNL_FAULT: '%s' armed", SITES[i]);
		}
		spec += len;
		if (*spec == ',')
			spec++;
	}
}

bool fault_hit(const char *name)
{
	const int i = site_index(name, strlen(name));
	FNL_ASSERT(i >= 0);
	if (i < 0 || !faults[i].armed)
		return false;

	Fault *f = &faults[i];
	const uint32_t hit = fnl_atomic_add_u32(&f->hits, 1) + 1;
	bool fire;
	switch (f->mode) {
	case NTH:    fire = hit == f->n;     break;
	case PERIOD: fire = hit % f->n == 0; break;
	case FIRST:  fire = hit <= f->n;     break;
	default:     fire = true;            break;
	}
	if (fire)
		fnl_log(FNL_LOG_WARN, "FNL_FAULT: '%s' fired (hit %u)", name,
				(unsigned)hit);
	return fire;
}

#endif /* NDEBUG */
