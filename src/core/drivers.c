/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * A GPU moved to another kernel driver with the window open: an NVIDIA GPU
 * between nouveau and NVIDIA's own module, which the kernel takes with no
 * reboot once nothing holds the GPU open (DESIGN *Another kernel driver,
 * with the window open*).
 *
 * The move is a renderer swap (renderer_swap()) with the driver changed
 * between the teardown and the build: a destroyed Vulkan instance lets the
 * GPU go, NVK's and NVIDIA's alike, and so does OpenGL with glvnd closed
 * (egl_dyn.c), so that is the one point in a window's life where the
 * process holds nothing of it. The window stays open across it, and goes
 * on drawing on the GPUs the move leaves alone while it runs -- on Vulkan,
 * whatever it lands on, since OpenGL would load NVIDIA's EGL -- the move
 * on a thread of its own (DESIGN *Drawn across the move*); where it
 * cannot, its frames pause for the move, a second or two. Asked for as a
 * GPU is, through the list: a GPU that can move is an entry more for each
 * renderer the caller has a scene for, "<GPU>, on <driver>", drawn with
 * that renderer after the move, which fnl_request_gpu() and the titlebar's
 * GPU box take like any other; with the GPU's own entries, each renderer
 * on each driver is one entry of the list.
 *
 * FNL_GPU_DRIVER turns it on: a driver's name binds that one before the
 * window first draws, and anything else ("switch") leaves the GPU as it is.
 * Off by default, since the entries are moves a round over the list would
 * otherwise make. The KMS backend's alone, where the program is the only
 * display client and so the only thing that can hold the GPU open; under a
 * window server the other driver's boot entry is the way.
 */
#include "core/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The GPU a move is of, as the list names it: the one listed Vulkan GPU
 * whose name says NVIDIA, without what a driver adds in brackets -- NVK's
 * "(NVK GP107)" -- so that the name holds across the move. Vulkan's
 * alone, since the move lands on Vulkan by that name, and OpenGL names
 * the same GPU otherwise ("/PCIe/SSE2"). With none listed, or more than
 * one, the address stands in. */
static void move_gpu_name(const GpuEntry *list, int n, const GpuDriver *g,
						  char *out, size_t size)
{
	const char *found = NULL;
	int seen = 0;
	for (int i = 0; i < n; i++)
		if (list[i].renderer == FNL_RENDERER_VULKAN &&
				app_name_has(list[i].name, "nvidia")) {
			found = list[i].name;
			seen++;
		}
	if (seen != 1) {
		snprintf(out, size, "NVIDIA GPU %s", g->pci);
		return;
	}
	const char *cut = strstr(found, " (");
	const int len = cut ? (int)(cut - found) : (int)strlen(found);
	snprintf(out, size, "%.*s", len, found);
}

/* Whether a landing on `driver` with renderer `r` found no GPU of the
 * moved one's name (app_gpu_driver_unseen()). Under App::lock, as
 * app_gpu_driver_entries() is, which gpu_merge() calls holding it: not
 * annotated, since app.h declares that one where App is incomplete. */
static bool move_unseen(const App *a, const char *driver, FnlRenderer r)
{
	for (int i = 0; i < a->gpu_driver_unseen_n; i++)
		if (a->gpu_driver_unseen[i].renderer == r &&
				strcmp(a->gpu_driver_unseen[i].driver, driver) == 0)
			return true;
	return false;
}

/* Whether the GPU listed as `name` is the one that can move, named `gpu`
 * by move_gpu_name(): every renderer's name for it holds Vulkan's --
 * NVIDIA's OpenGL behind it ("/PCIe/SSE2"), Zink's around it. An address
 * names no listed GPU. */
static bool names_moving(const char *name, const char *gpu)
{
	return gpu[0] && strncmp(gpu, "NVIDIA GPU ", 11) != 0 &&
		   app_name_has(name, gpu);
}

/* Whether a move is offered: FNL_GPU_DRIVER set, one GPU that can move
 * and another driver for it, and nothing found holding NVIDIA's driver.
 * Under App::lock. */
static bool moves_offered(const App *a)
{
	return a->gpu_drivers_on && !a->gpu_drivers_pinned &&
		   a->gpu_drivers_n == 1 && a->gpu_drivers[0].other[0];
}

/* Whether the GPU's rows are swapped with its move entries: it is on a
 * driver other than its first, and the move offered is the one back. */
static bool moves_swapped(const App *a)
{
	const GpuDriver *g = &a->gpu_drivers[0];
	return moves_offered(a) && a->gpu_driver_home[0] &&
		   strcmp(g->bound, a->gpu_driver_home) != 0 &&
		   strcmp(g->other, a->gpu_driver_home) == 0;
}

/* For the one GPU that can move (app_gpu_drivers_start()), an entry for
 * each renderer of the list (App::gpu_span) that has not missed on the
 * other driver, and the driver bound written into the GPU's own entries;
 * none once the moves are pinned or the list made again after a move finds
 * it with no other driver to go to.
 *
 * Each renderer on each driver keeps its number for the window's life, as
 * the list promises of every entry: the driver the GPU was on when a move
 * was first offered has the rows where the renderers list the GPU, and
 * the other the entries at the end. Away from that driver the GPU's row
 * of each renderer and that renderer's move entry, the way back, trade
 * places. Both laptops' renderers list the GPU at one place on either
 * driver, first; where one did not, its row there would follow it. */
int app_gpu_driver_entries(App *a, GpuEntry *list, int n, int max)
{
	a->gpu_driver_gpu[0] = '\0';
	if (!moves_offered(a))
		return 0;
	const GpuDriver *g = &a->gpu_drivers[0];
	if (!a->gpu_driver_home[0])
		snprintf(a->gpu_driver_home, sizeof a->gpu_driver_home, "%s",
				 g->bound);
	char gpu[GPU_NAME_MAX - 32];
	move_gpu_name(list, n, g, gpu, sizeof gpu);
	snprintf(a->gpu_driver_gpu, sizeof a->gpu_driver_gpu, "%s", gpu);
	for (int i = 0; i < n; i++)
		if (names_moving(list[i].name, gpu))
			snprintf(list[i].driver, sizeof list[i].driver, "%s", g->bound);
	const bool swap = moves_swapped(a);
	int m = 0;
	for (int k = 0; k < a->gpu_span_n && n + m < max; k++) {
		const FnlRenderer r = a->gpu_span[k]->api;
		if (move_unseen(a, g->other, r))
			continue;
		/* The renderer's own row of the GPU, whose kind the entry takes:
		 * OpenGL's says no more than hardware. */
		int own = -1;
		for (int i = 0; i < n; i++)
			if (list[i].renderer == r && list[i].driver[0])
				own = i;
		/* Afresh: the slot may hold an entry of an earlier list, whose
		 * `via` and `way_default` no move has. */
		GpuEntry *e = &list[n + m++];
		*e = (GpuEntry){ 0 };
		snprintf(e->name, sizeof e->name, "%s, on %s", gpu, g->other);
		e->kind = own >= 0 ? list[own].kind : FNL_GPU_KIND_DISCRETE;
		e->renderer = r;
		e->driver_move = 1;
		snprintf(e->driver, sizeof e->driver, "%s", g->other);
		e->slot = e->local = -1;
		if (swap && own >= 0) {
			const GpuEntry t = list[own];
			list[own] = *e;
			*e = t;
		}
	}
	return m;
}

/* What the backend lists, published where gpu_merge() reads it: the list
 * takes it at the next renderer's publish, which every move is followed
 * by -- onto the GPU moved, or back. */
static void drivers_relist(App *a)
{
	GpuDriver got[GPU_DRIVERS_MAX];
	int n = BACKEND(a)->gpu_drivers(a, got, GPU_DRIVERS_MAX);
	if (n < 0)
		n = 0;
	fnl_mutex_lock(&a->lock);
	memcpy(a->gpu_drivers, got, (size_t)n * sizeof *got);
	a->gpu_drivers_n = n;
	fnl_mutex_unlock(&a->lock);
}

/* The driver GPU `pci` is on in the list last made, into `out`; "none"
 * where it is not listed. */
static void bound_now(App *a, const char *pci, char *out, size_t size)
{
	snprintf(out, size, "none");
	fnl_mutex_lock(&a->lock);
	for (int i = 0; i < a->gpu_drivers_n; i++)
		if (strcmp(a->gpu_drivers[i].pci, pci) == 0)
			snprintf(out, size, "%s", a->gpu_drivers[i].bound);
	fnl_mutex_unlock(&a->lock);
}

/* The GPU onto `driver` and the list made again, judged by what is bound
 * after -- into `now` -- and not by the helper's status alone, which a
 * caller that reaps its own children has taken; said where the two
 * differ. */
static bool bind_and_check(App *a, const GpuDriver *g, const char *driver,
						   char *now, size_t size)
{
	const bool said = BACKEND(a)->gpu_driver_set(a, g, driver);
	drivers_relist(a);
	bound_now(a, g->pci, now, size);
	const bool on = strcmp(now, driver) == 0;
	if (!on)
		fnl_log(FNL_LOG_WARN, "GPU driver: %s is on %s, not %s%s", g->pci,
				now, driver, said ? ", though the helper said it was" : "");
	return on;
}

void app_gpu_drivers_start(App *a)
{
	a->gpu_driver_next = -1;
	const char *v = getenv("FNL_GPU_DRIVER");
	if (!v || !v[0])
		return;
	if (BACKEND(a)->gpu_drivers == backend_noop_gpu_drivers) {
		fnl_log(FNL_LOG_WARN, "FNL_GPU_DRIVER: a move to another kernel "
				"driver is the KMS backend's alone; under %s, boot the "
				"other driver's entry", BACKEND(a)->name);
		return;
	}
	if (!renderer_of_api(FNL_RENDERER_VULKAN)) {
		fnl_log(FNL_LOG_WARN, "FNL_GPU_DRIVER: this build has no Vulkan, "
				"which a move to another kernel driver is made on");
		return;
	}
	drivers_relist(a);
	if (a->gpu_drivers_n == 0) {
		fnl_log(FNL_LOG_INFO, "FNL_GPU_DRIVER: no GPU here can be moved to "
				"another kernel driver");
		return;
	}
	for (int k = 0; k < a->gpu_drivers_n; k++) {
		const GpuDriver *g = &a->gpu_drivers[k];
		fnl_log(FNL_LOG_INFO, "GPU driver: %s is on %s%s%s", g->pci, g->bound,
				g->other[0] ? ", and can be moved to " : "", g->other);
	}
	/* One GPU, and the move never cut short (DESIGN): a module unloaded is
	 * unloaded from every GPU it drives, and which of two NVIDIA GPUs a
	 * name picks after the move is the driver's order. */
	if (a->gpu_drivers_n > 1) {
		fnl_log(FNL_LOG_WARN, "FNL_GPU_DRIVER: %d NVIDIA GPUs, and a module "
				"moves every GPU it drives at once: no move is offered",
				a->gpu_drivers_n);
		return;
	}
	const GpuDriver g = a->gpu_drivers[0];
	if (!g.other[0]) {
		fnl_log(FNL_LOG_INFO, "FNL_GPU_DRIVER: this kernel has no other "
				"driver for %s: no move is offered", g.pci);
		return;
	}
	a->gpu_drivers_on = true;
#ifndef _WIN32
	/* nouveau's own OpenGL makes no 4.5 core context on the GPUs measured
	 * (GM107, GP107: EGL_BAD_MATCH), and Zink on NVK makes one and lets the
	 * GPU go with its display, as any Vulkan device does: so OpenGL draws
	 * on a GPU under nouveau through Zink, Mesa's loader's switch for
	 * nouveau alone. The one variable the library writes, and only with
	 * the moves offered and the caller's own value left: set here, on the
	 * caller's thread before the library has one of its own, and before
	 * any renderer -- Mesa reads it at each display it opens. */
	setenv("NOUVEAU_USE_ZINK", "1", 0);
#endif
	/* A driver named is bound now, where the GPU is not on it -- from
	 * nothing as well, which a move cut short leaves and whose list names
	 * one other driver of the two. The backend refuses a name that is no
	 * driver's. */
	char now[GPU_DRIVER_NAME_MAX];
	if (strcmp(g.bound, v) != 0 &&
		(strcmp(g.other, v) == 0 || strcmp(g.bound, "none") == 0)) {
		(void)bind_and_check(a, &g, v, now, sizeof now);
		return;
	}
	/* From nouveau with NVIDIA's module not loaded, the module is loaded
	 * by a move there and straight back, before any renderer: NVIDIA's
	 * Vulkan and EGL drivers try to load it at each instance and display,
	 * 0.85 s a try while nouveau holds the GPU, nine of them before the
	 * first frame (13.6 s on aspire, 19.5 s on schenker, against 3.6 with
	 * the module loaded), and it will not load beside nouveau ("No such
	 * device"). Loaded and let go it stays, and costs 30 ms an instance.
	 * Its first move onto the GPU then binds and does not load. */
	if (strcmp(g.bound, "nouveau") == 0 && strcmp(g.other, "nvidia") == 0 &&
			!g.other_loaded) {
		fnl_log(FNL_LOG_INFO, "GPU driver: NVIDIA's module is not loaded, "
				"which NVIDIA's userspace then tries at every Vulkan "
				"instance; loaded by a move to it and back");
		if (!bind_and_check(a, &g, "nvidia", now, sizeof now))
			return;
		fnl_mutex_lock(&a->lock);
		GpuDriver back = a->gpu_drivers[0];
		fnl_mutex_unlock(&a->lock);
		(void)bind_and_check(a, &back, "nouveau", now, sizeof now);
	}
}

/* The swap onto the renderer the move lands on owed to the next recovery,
 * drawn on the GPU gpu_want names -- the moved one -- as gpu_switch_owed()
 * owes a move to another renderer's GPU. The choice before (gpu_back_*)
 * is the caller's: app_gpu_driver_owe() keeps it once, and the swap after
 * a cross goes back to the same. */
static void swap_owed(App *a)
{
	a->gpu_index = 0;
	a->renderer_next = a->gpu_driver_to;
	a->gpu_switching = true;
	a->gpu_switch_back = false;
	a->surface_lost = true;
}

void app_gpu_driver_owe(App *a, int move, const char *name,
						FnlRenderer renderer)
{
	/* The list holds the renderers the caller has a scene for, each one
	 * the build has. */
	a->gpu_driver_to = renderer_of_api(renderer);
	FNL_ASSERT(a->gpu_driver_to != NULL);
	fnl_log(FNL_LOG_INFO, "moving to %s, the renderer built again on it "
			"(%s)", name, a->gpu_driver_to->name);
	a->gpu_back_index = a->gpu_index;
	memcpy(a->gpu_back_want, a->gpu_want, sizeof a->gpu_back_want);
	/* Drawn on that GPU after the move, by the name it keeps on either
	 * driver; where the list could only give its address, by its maker. */
	const char *cut = strstr(name, ", on ");
	const int len = cut ? (int)(cut - name) : (int)strlen(name);
	if (strncmp(name, "NVIDIA GPU ", 11) == 0)
		snprintf(a->gpu_want, sizeof a->gpu_want, "NVIDIA");
	else
		snprintf(a->gpu_want, sizeof a->gpu_want, "%.*s", len, name);
	a->gpu_driver_next = move;
	swap_owed(a);
}

bool app_gpu_driver_take(App *a, GpuDriver *g)
{
	const int k = a->gpu_driver_next;
	a->gpu_driver_next = -1;
	fnl_mutex_lock(&a->lock);
	const bool known = k >= 0 && k < a->gpu_drivers_n;
	if (known)
		*g = a->gpu_drivers[k];
	fnl_mutex_unlock(&a->lock);
	if (!known || !g->other[0])
		return false;
	/* The other renderers' listing has an instance of its own, which holds
	 * the GPU as the renderer's did, for as long as it runs. It runs again
	 * once the move has landed (renderer_swap()), and not where the window
	 * comes back to where it was. */
	if (a->lister_started) {
		fnl_thread_join(a->lister);
		a->lister_started = false;
	}
	return true;
}

/* On the render thread with nothing built, or on the mover's while the
 * render thread draws across: the move's fault site, the helper and the
 * list are none of them the render thread's. */
bool app_gpu_driver_make(App *a, const GpuDriver *g)
{
	if (FNL_FAULT("gpu-driver-refused")) {
		fnl_log(FNL_LOG_WARN, "GPU driver: %s stays on %s (FNL_FAULT)",
				g->pci, g->bound);
		a->gpu_driver_moved = false;
		return false;
	}
	char now[GPU_DRIVER_NAME_MAX];
	const bool moved = bind_and_check(a, g, g->other, now, sizeof now);
	/* Anything changed is put back if the window does not land: the GPU on
	 * the driver asked for, or a helper cut short between letting go and
	 * binding, the GPU on none. */
	a->gpu_driver_made = *g;
	a->gpu_driver_moved = strcmp(now, g->bound) != 0;
	return moved;
}

/* The move on a thread of its own, while the render thread draws across
 * it. Its end wakes a loop at rest, which would otherwise not look for it
 * until something else asked for a frame. */
static void *mover_main(void *arg)
{
	App *a = arg;
	a->mover_moved = app_gpu_driver_make(a, &a->mover_move);
	fnl_mutex_lock(&a->lock);
	fnl_atomic_store_bool(&a->mover_done, true);
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
	return NULL;
}

bool app_gpu_driver_start_move(App *a, const GpuDriver *g)
{
	a->mover_move = *g;
	a->mover_moved = false;
	fnl_atomic_store_bool(&a->mover_done, false);
	a->mover_started = fnl_thread_start(&a->mover, mover_main, a);
	return a->mover_started;
}

void app_gpu_driver_poll(App *a)
{
	if (!a->mover_started || !fnl_atomic_load_bool(&a->mover_done))
		return;
	/* The landing is a move of its own on screen, so the veil comes down
	 * over the frames drawn across before it (app_veil_gate()); the
	 * thread, done, waits for that. */
	if (!app_veil_gate(a))
		return;
	fnl_thread_join(a->mover);
	a->mover_started = false;
	/* The move has ended, moved or not: the next instance may load every
	 * driver again, and the window goes to the GPU moved, by its name,
	 * where renderer_swap() takes the mover's answer for a move of its
	 * own. Back to the GPU before (gpu_back_*, as app_gpu_driver_owe()
	 * left them) where it did not move or will not draw. */
	a->gpu_cross = false;
	a->gpu_cross_landing = true;
	memcpy(a->gpu_want, a->gpu_cross_want, sizeof a->gpu_want);
	swap_owed(a);
}

void app_gpu_driver_join(App *a)
{
	if (!a->mover_started)
		return;
	fnl_thread_join(a->mover);
	a->mover_started = false;
}

bool app_gpu_driver_landed(App *a)
{
	if (!a->gpu_driver_moved)
		return true;
	if (FNL_FAULT("gpu-driver-missed"))
		return false;
	/* gpu_want is the moved GPU's name (app_gpu_driver_owe()), and a pick
	 * whose name matches nothing draws on another GPU and says so with a
	 * line: the window would then be on Intel's, the GPU moved for
	 * nothing. */
	return app_name_has(a->gpu_name, a->gpu_want);
}

void app_gpu_driver_prefer(App *a, const char *driver)
{
	if (!a->gpu_drivers_on || a->gpu_drivers_pinned)
		return;
	fnl_mutex_lock(&a->lock);
	const bool one = a->gpu_drivers_n == 1;
	GpuDriver g = a->gpu_drivers[0];
	fnl_mutex_unlock(&a->lock);
	if (!one || strcmp(g.bound, driver) == 0 || strcmp(g.other, driver) != 0)
		return;
	char now[GPU_DRIVER_NAME_MAX];
	(void)bind_and_check(a, &g, driver, now, sizeof now);
}

bool app_gpu_driver_offered(App *a, GpuDriver *g)
{
	fnl_mutex_lock(&a->lock);
	const bool on = moves_offered(a);
	if (on)
		*g = a->gpu_drivers[0];
	fnl_mutex_unlock(&a->lock);
	return on;
}

const char *app_gpu_driver_of(const char *name)
{
	/* Every driver a move is between (tools/gpu-driver.sh), offered or
	 * not: a key kept by a run with the moves names one, and a run
	 * without them must still tell it from a GPU's own name. */
	static const char *const DRIVERS[] = { "nouveau", "nvidia" };
	const size_t n = strlen(name);
	for (size_t i = 0; i < sizeof DRIVERS / sizeof *DRIVERS; i++) {
		const size_t t = strlen(DRIVERS[i]);
		if (n > t + 4 && strcmp(name + n - t, DRIVERS[i]) == 0 &&
				strncmp(name + n - t - 4, " on ", 4) == 0)
			return name + n - t;
	}
	return NULL;
}

void app_gpu_driver_unseen(App *a)
{
	const GpuDriver *made = &a->gpu_driver_made;
	char now[GPU_DRIVER_NAME_MAX];
	bound_now(a, made->pci, now, sizeof now);
	if (strcmp(now, made->other) != 0)
		return;
	/* The renderer landed on, which the swap has torn down again. */
	const FnlRendererVtbl *to = a->gpu_driver_to;
	const FnlRenderer r = to->api;
	fnl_mutex_lock(&a->lock);
	const int k = a->gpu_driver_unseen_n;
	const bool add = k < (int)(sizeof a->gpu_driver_unseen /
							   sizeof a->gpu_driver_unseen[0]) &&
					 !move_unseen(a, made->other, r);
	if (add) {
		snprintf(a->gpu_driver_unseen[k].driver,
				 sizeof a->gpu_driver_unseen[k].driver, "%s", made->other);
		a->gpu_driver_unseen[k].renderer = r;
		a->gpu_driver_unseen_n = k + 1;
	}
	fnl_mutex_unlock(&a->lock);
	if (!add)
		return;
	app_gpu_list_merge(a);
	fnl_log(FNL_LOG_WARN, "GPU driver: no move to %s with %s offered from "
			"now on: on %s the GPU is listed by no %s driver -- that "
			"driver's userspace not installed, or not seeing it",
			made->other, to->name, made->other, to->name);
}

void app_gpu_driver_undo(App *a)
{
	if (!a->gpu_driver_moved)
		return;
	a->gpu_driver_moved = false;
	const GpuDriver made = a->gpu_driver_made;
	/* From no driver there is nothing to go back to: a GPU on none is one
	 * nothing draws on, so it keeps the driver it was given. */
	if (strcmp(made.bound, "none") == 0) {
		char made_now[GPU_DRIVER_NAME_MAX];
		bound_now(a, made.pci, made_now, sizeof made_now);
		fnl_log(FNL_LOG_INFO, "GPU driver: %s stays on %s, having had no "
				"driver before", made.pci, made_now);
		return;
	}
	GpuDriver back = made;
	bound_now(a, made.pci, back.bound, sizeof back.bound);
	snprintf(back.other, sizeof back.other, "%s", made.bound);
	fnl_log(FNL_LOG_WARN, "GPU driver: %s back from %s to %s", made.pci,
			back.bound, made.bound);
	char now[GPU_DRIVER_NAME_MAX];
	(void)bind_and_check(a, &back, made.bound, now, sizeof now);
}

/* ---- a GPU plugged in or out ------------------------------------------- */

#define GPU_HOTPLUG_WAIT 1.5    /* seconds, for udev to finish the nodes */

bool app_gpu_driver_owns(const App *a, const char *devpath)
{
	App *m = (App *)a;                 /* the lock is not const */
	bool owns = false;
	fnl_mutex_lock(&m->lock);
	for (int k = 0; k < a->gpu_drivers_n && !owns; k++) {
		char at[GPU_PCI_MAX + 2];
		snprintf(at, sizeof at, "/%s/", a->gpu_drivers[k].pci);
		owns = strstr(devpath, at) != NULL;
	}
	fnl_mutex_unlock(&m->lock);
	return owns;
}

void app_gpu_hotplug(App *a)
{
	fnl_log(FNL_LOG_INFO, "GPU hotplug: a GPU came or went; the list is "
			"made again");
	(void)fnl_atomic_add_u32(&a->gpu_hotplug_gen, 1);
	fnl_mutex_lock(&a->lock);
	a->want_user_gen++;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
}

/* The list made again where it can be: not while a move, a swap or a
 * calibration is under way, which have the GPU list as it is; and after
 * the veil, as any move to a GPU is (gpu_switch_owed()). The rebuild is on
 * the GPU in use, by its name -- its place in the list may be another's
 * now -- and lists the other renderers first, as a landing on another
 * kernel driver does (gpu_relist_landing()): listed after the first frame
 * instead, a Vulkan instance beside NVIDIA's live EGL display would end
 * it. A GPU in use that went is a lost device first, which the recovery
 * answers on another; the list is made again after it all the same. */
void app_gpu_hotplug_poll(App *a)
{
	const uint32_t gen = fnl_atomic_load_u32(&a->gpu_hotplug_gen);
	if (gen == a->gpu_hotplug_done || !a->renderer ||
		!a->renderer_ready || a->mover_started || a->gpu_cross_landing ||
		a->gpu_switching || a->renderer_next || app_rank_holds_input(a))
		return;
	/* The kernel's uevent comes before udev has given the new card's
	 * nodes their group, which a driver listing it at once would be
	 * refused: GPU_HOTPLUG_WAIT after the render loop first sees the
	 * last one heard, the wait begun again by each that joins it. One
	 * heard once the veil is coming down waits for a turn of its own
	 * instead: begun again then, the wait would hold the veil down. */
	const double now = now_seconds();
	if (a->gpu_hotplug_seen <= 0.0 ||
		(gen != a->gpu_hotplug_seen_gen &&
		 now - a->gpu_hotplug_seen < GPU_HOTPLUG_WAIT)) {
		a->gpu_hotplug_seen = now;
		a->gpu_hotplug_seen_gen = gen;
	}
	if (now - a->gpu_hotplug_seen < GPU_HOTPLUG_WAIT)
		return;
	if (!app_veil_gate(a))
		return;
	a->gpu_hotplug_seen = 0.0;
	/* Up to the one waited for: any heard since is owed a turn of its own. */
	a->gpu_hotplug_done = a->gpu_hotplug_seen_gen;
	a->gpu_back_index = a->gpu_index;
	memcpy(a->gpu_back_want, a->gpu_want, sizeof a->gpu_back_want);
	snprintf(a->gpu_want, sizeof a->gpu_want, "%.*s",
			 (int)sizeof a->gpu_want - 1, a->gpu_name);
	a->gpu_index = 0;
	a->renderer_next = a->renderer;
	a->gpu_hotplug_relist = true;
	a->gpu_switching = true;
	a->gpu_switch_back = false;
	a->surface_lost = true;
}
