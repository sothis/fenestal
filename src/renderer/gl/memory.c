/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The GPU memory this process holds, for the metrics panel
 * (FnlRendererVtbl::gpu_memory), where OpenGL keeps no such figure of one
 * process's: asked of the system instead, on Linux. NVIDIA's own driver
 * through its management library, NVML, opened at run time
 * (libnvidia-ml.so.1, there wherever the driver is), which lists each
 * process drawing on the GPU and what it holds, as nvidia-smi shows it; and
 * Mesa's drivers through the kernel's own account of each DRM client,
 * /proc/self/fdinfo, where the driver keeps one (i915, xe, amdgpu), summed
 * over this process's clients of it. nouveau keeps none, so Zink on NVK has
 * no figure, nor has CGL on a Mac.
 *
 * NVML started holds NVIDIA's driver -- /dev/nvidiactl, nvidia0,
 * nvidia-uvm -- and the GPU will not move to nouveau (FNL_GPU_DRIVER)
 * while anything does; so it is started only when the panel first asks and
 * shut down with the device (gl_memory_release()), which every move
 * rebuilds. Render thread only.
 */

#include "core/app.h"

#include "core/dynlib.h"
#include "renderer/gl/gl_internal.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>

/* ---- NVIDIA's own driver, through NVML ---------------------------------- */

/* The few NVML types and calls used, as nvml.h declares them; the header is
 * not installed with the driver, and the library is opened, not linked. */
typedef struct nvmlDevice_st *nvmlDevice_t;
typedef struct {
	unsigned int       pid;
	unsigned long long usedGpuMemory;
	unsigned int       gpuInstanceId;
	unsigned int       computeInstanceId;
} nvmlProcessInfo_v2_t;
#define NVML_SUCCESS             0
#define NVML_VALUE_NOT_AVAILABLE (~0ULL)
#define NVML_PROCS_MAX           64

typedef int (*nvml_init_fn)(void);
typedef int (*nvml_shutdown_fn)(void);
typedef int (*nvml_count_fn)(unsigned int *);
typedef int (*nvml_handle_fn)(unsigned int, nvmlDevice_t *);
typedef int (*nvml_procs_fn)(nvmlDevice_t, unsigned int *,
							 nvmlProcessInfo_v2_t *);

/* NVML while it is started: from the first figure asked of it to the
 * device's end. `tried` keeps a machine without it from being asked again
 * every sample, and a device's end clears it only where the library
 * opened, so that the next device starts NVML again. The library itself
 * stays open (core/dynlib.h). */
static struct {
	bool             tried, ready;
	void            *lib;
	nvml_init_fn     init;
	nvml_shutdown_fn shutdown;
	nvml_count_fn    count;
	nvml_handle_fn   handle;
	nvml_procs_fn    procs;
} g_nvml;

/* One symbol into its typed pointer: fnl_lib_sym() hands back a generic
 * function pointer, and the copy names no cast between pointer kinds. */
static void nvml_sym(const char *name, void *out, size_t size)
{
	FnlLibFn f = fnl_lib_sym(g_nvml.lib, name);
	memcpy(out, &f, size);
}

static bool nvml_start(void)
{
	if (g_nvml.tried)
		return g_nvml.ready;
	g_nvml.tried = true;
	if (!g_nvml.lib) {
		static const char *const NAMES[] = { "libnvidia-ml.so.1", NULL };
		char why[160];
		g_nvml.lib = fnl_lib_open(NAMES, why, sizeof why);
		if (!g_nvml.lib)
			return false;
		nvml_sym("nvmlInit_v2", &g_nvml.init, sizeof g_nvml.init);
		nvml_sym("nvmlShutdown", &g_nvml.shutdown, sizeof g_nvml.shutdown);
		nvml_sym("nvmlDeviceGetCount_v2", &g_nvml.count,
				 sizeof g_nvml.count);
		nvml_sym("nvmlDeviceGetHandleByIndex_v2", &g_nvml.handle,
				 sizeof g_nvml.handle);
		nvml_sym("nvmlDeviceGetGraphicsRunningProcesses_v2", &g_nvml.procs,
				 sizeof g_nvml.procs);
	}
	g_nvml.ready = g_nvml.init && g_nvml.shutdown && g_nvml.count &&
				   g_nvml.handle && g_nvml.procs &&
				   g_nvml.init() == NVML_SUCCESS;
	if (!g_nvml.ready)
		fnl_log(FNL_LOG_INFO, "metrics: NVML would not start; no GPU memory "
				"figure on this NVIDIA GPU");
	return g_nvml.ready;
}

/* This process's memory on any NVIDIA GPU, summed; false where NVML is not
 * there, will not start, or does not list this process. */
static bool nvml_mib(double *mib)
{
	unsigned int n = 0;
	if (!nvml_start() || g_nvml.count(&n) != NVML_SUCCESS)
		return false;
	const unsigned int me = (unsigned int)getpid();
	unsigned long long used = 0;
	bool listed = false;
	for (unsigned int d = 0; d < n; d++) {
		nvmlDevice_t dev;
		nvmlProcessInfo_v2_t info[NVML_PROCS_MAX];
		unsigned int k = NVML_PROCS_MAX;
		if (g_nvml.handle(d, &dev) != NVML_SUCCESS ||
			g_nvml.procs(dev, &k, info) != NVML_SUCCESS)
			continue;
		for (unsigned int i = 0; i < k && i < NVML_PROCS_MAX; i++)
			if (info[i].pid == me &&
				info[i].usedGpuMemory != NVML_VALUE_NOT_AVAILABLE) {
				used += info[i].usedGpuMemory;
				listed = true;
			}
	}
	if (!listed)
		return false;
	*mib = (double)used / (1024.0 * 1024.0);
	return true;
}

/* ---- Mesa's drivers, through the kernel's fdinfo ------------------------ */

#define FDINFO_CLIENTS_MAX 64

/* This process's memory held through DRM clients of `driver`, summed once
 * a client -- a descriptor dup'd is the same client, by drm-client-id --
 * over every region the driver names (drm-total-<region>); false where no
 * client of it says any. */
static bool fdinfo_mib(const char *driver, double *mib)
{
	DIR *d = opendir("/proc/self/fdinfo");
	if (!d)
		return false;
	unsigned long seen[FDINFO_CLIENTS_MAX];
	int seen_n = 0;
	double kib = 0.0;
	bool any = false;
	const struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[300];
		snprintf(path, sizeof path, "/proc/self/fdinfo/%s", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		char line[256], drv[64] = "";
		unsigned long id = 0;
		double total = 0.0;
		bool has_id = false;
		while (fgets(line, sizeof line, f)) {
			char unit[8] = "";
			double v;
			if (sscanf(line, "drm-driver: %63s", drv) == 1)
				continue;
			if (sscanf(line, "drm-client-id: %lu", &id) == 1) {
				has_id = true;
				continue;
			}
			const char *colon = strchr(line, ':');
			if (strncmp(line, "drm-total-", 10) == 0 && colon &&
				sscanf(colon + 1, "%lf %7s", &v, unit) >= 1)
				total += strcmp(unit, "MiB") == 0 ? v * 1024.0
					   : strcmp(unit, "KiB") == 0 ? v : v / 1024.0;
		}
		fclose(f);
		if (strcmp(drv, driver) != 0 || !has_id)
			continue;
		bool again = false;
		for (int i = 0; i < seen_n && !again; i++)
			again = seen[i] == id;
		if (again)
			continue;
		if (seen_n < FDINFO_CLIENTS_MAX)
			seen[seen_n++] = id;
		kib += total;
		any = true;
	}
	closedir(d);
	if (!any)
		return false;
	*mib = kib / 1024.0;
	return true;
}
#endif

void gl_memory_release(void)
{
#if defined(__linux__)
	if (g_nvml.ready)
		(void)g_nvml.shutdown();
	g_nvml.ready = false;
	if (g_nvml.lib)
		g_nvml.tried = false;
#endif
}

bool gl_gpu_memory(App *a, double *mib)
{
#if defined(__linux__)
	/* By the name the context gives the GPU: NVIDIA's own driver names it
	 * plainly, Mesa's put their own in front ("Mesa Intel(R)", "zink"). */
	const char *gpu = RENDERER(a)->device_name(a);
	if (strstr(gpu, "NVIDIA") && !strstr(gpu, "zink") && !strstr(gpu, "NVK"))
		return nvml_mib(mib);
	if (strstr(gpu, "Intel"))
		return fdinfo_mib("i915", mib) || fdinfo_mib("xe", mib);
	if (strstr(gpu, "AMD") || strstr(gpu, "Radeon"))
		return fdinfo_mib("amdgpu", mib);
	return false;
#else
	(void)a;
	(void)mib;
	return false;
#endif
}
