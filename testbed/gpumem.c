/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The GPU memory this process holds where the graphics API keeps no such
 * figure -- OpenGL, which has none of one process's -- asked of the system
 * instead, on Linux: NVIDIA's own driver through its management library,
 * NVML, opened at run time (libnvidia-ml.so.1, there wherever the driver
 * is), which lists each process drawing on the GPU and what it holds, as
 * nvidia-smi shows it; and Mesa's drivers through the kernel's own account
 * of each DRM client, /proc/self/fdinfo, where the driver keeps one (i915,
 * xe, amdgpu), its memory summed over this process's clients of it. nouveau
 * keeps none, so Zink on NVK has no figure. Other systems: none here.
 */
#if !defined(_WIN32) && !defined(__APPLE__)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <dirent.h>
#include <dlfcn.h>
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
#define NVML_SUCCESS                0
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_VALUE_NOT_AVAILABLE    (~0ULL)

typedef int (*nvml_init_fn)(void);
typedef int (*nvml_shutdown_fn)(void);
typedef int (*nvml_count_fn)(unsigned int *);
typedef int (*nvml_handle_fn)(unsigned int, nvmlDevice_t *);
typedef int (*nvml_procs_fn)(nvmlDevice_t, unsigned int *,
							 nvmlProcessInfo_v2_t *);

/* NVML while it is started: from the first figure asked of it to the
 * device's end (demo_gpu_mem_os_release()). Started, it holds NVIDIA's
 * driver -- /dev/nvidiactl, nvidia0, nvidia-uvm -- and the library will
 * not move the GPU to nouveau (FNL_GPU_DRIVER) while anything does; shut
 * down it lets go of all of it (schenker, 580.196). Only the render
 * thread, and the thread fnl_run() was called on once that one has
 * ended, reach it. */
static bool             nvml_tried, nvml_ready;
static void            *nvml_lib;
static nvml_shutdown_fn nvml_shutdown;
static nvml_count_fn    nvml_count;
static nvml_handle_fn   nvml_handle;
static nvml_procs_fn    nvml_procs;

/* This process's memory on `dev` added to *used, off a list with room for
 * `cap` processes; NVML's answer, and its count in *need where the list is
 * longer. The list is the call's own, freed in it, never kept across the
 * caller's retries: -fanalyzer loses a heap pointer stored inside a loop. */
static int nvml_sum(nvmlDevice_t dev, unsigned int cap, unsigned int me,
					unsigned int *need, unsigned long long *used,
					bool *listed)
{
	nvmlProcessInfo_v2_t stack[64];
	nvmlProcessInfo_v2_t *info =
		cap <= 64 ? stack : malloc(cap * sizeof stack[0]);
	if (!info)
		return -1;
	unsigned int k = cap;
	const int rc = nvml_procs(dev, &k, info);
	if (rc == NVML_SUCCESS) {
		for (unsigned int i = 0; i < k && i < cap; i++)
			if (info[i].pid == me &&
				info[i].usedGpuMemory != NVML_VALUE_NOT_AVAILABLE) {
				*used += info[i].usedGpuMemory;
				*listed = true;
			}
	} else if (rc == NVML_ERROR_INSUFFICIENT_SIZE) {
		*need = k;
	}
	if (info != stack)
		free(info);
	return rc;
}

/* This process's memory on any NVIDIA GPU, summed; false where NVML is not
 * there, will not start, or does not list this process. */
static bool nvml_mib(double *mib)
{
	if (!nvml_tried) {
		nvml_tried = true;
		nvml_lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
		if (!nvml_lib)
			return false;
		nvml_init_fn init = (nvml_init_fn)dlsym(nvml_lib, "nvmlInit_v2");
		nvml_shutdown = (nvml_shutdown_fn)dlsym(nvml_lib, "nvmlShutdown");
		nvml_count = (nvml_count_fn)dlsym(nvml_lib, "nvmlDeviceGetCount_v2");
		nvml_handle = (nvml_handle_fn)dlsym(nvml_lib,
					"nvmlDeviceGetHandleByIndex_v2");
		nvml_procs = (nvml_procs_fn)dlsym(nvml_lib,
					"nvmlDeviceGetGraphicsRunningProcesses_v2");
		nvml_ready = init && nvml_shutdown && nvml_count && nvml_handle &&
					 nvml_procs && init() == NVML_SUCCESS;
	}
	unsigned int n = 0;
	if (!nvml_ready || nvml_count(&n) != NVML_SUCCESS)
		return false;
	const unsigned int me = (unsigned int)getpid();
	unsigned long long used = 0;
	bool listed = false;
	for (unsigned int d = 0; d < n; d++) {
		nvmlDevice_t dev;
		if (nvml_handle(d, &dev) != NVML_SUCCESS)
			continue;
		/* More processes on the GPU than the first list holds: asked again
		 * with the count NVML gave back, and room for a few started since.
		 * A bounded number of times and size, so that a list that keeps
		 * growing cannot hold the frame that asked. */
		unsigned int cap = 64;
		for (int tries = 0; tries < 4; tries++) {
			unsigned int need = 0;
			if (nvml_sum(dev, cap, me, &need, &used, &listed) !=
					NVML_ERROR_INSUFFICIENT_SIZE || need >= 65536)
				break;
			cap = need + 16;
		}
	}
	if (!listed)
		return false;
	*mib = (double)used / (1024.0 * 1024.0);
	return true;
}

/* ---- Mesa's drivers, through the kernel's fdinfo ------------------------ */

/* This process's memory held through DRM clients of `driver`, summed once
 * a client -- a descriptor dup'd is the same client, by drm-client-id --
 * over every region the driver names (drm-total-<region>); false where no
 * client of it says any. */
static bool fdinfo_mib(const char *driver, double *mib)
{
	DIR *d = opendir("/proc/self/fdinfo");
	if (!d)
		return false;
	unsigned long seen[64];
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
		if (seen_n < 64)
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

void demo_gpu_mem_os_release(void)
{
#if defined(__linux__)
	if (nvml_ready)
		(void)nvml_shutdown();
	if (nvml_lib)
		dlclose(nvml_lib);
	nvml_lib = NULL;
	nvml_tried = nvml_ready = false;
#endif
}

bool demo_gpu_mem_os(const char *gpu, double *mib)
{
#if defined(__linux__)
	/* By the name the renderer gives the GPU: NVIDIA's own driver names it
	 * plainly, Mesa's put their own in front ("Mesa Intel(R)", "zink"). */
	if (strstr(gpu, "NVIDIA") && !strstr(gpu, "zink") && !strstr(gpu, "NVK"))
		return nvml_mib(mib);
	if (strstr(gpu, "Intel"))
		return fdinfo_mib("i915", mib) || fdinfo_mib("xe", mib);
	if (strstr(gpu, "AMD") || strstr(gpu, "Radeon"))
		return fdinfo_mib("amdgpu", mib);
	return false;
#else
	(void)gpu;
	(void)mib;
	return false;
#endif
}
