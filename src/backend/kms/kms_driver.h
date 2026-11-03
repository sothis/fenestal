/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The KMS backend's FnlBackendVtbl::gpu_drivers, ::gpu_driver_set and
 * ::gpu_driver_held: the root helper that moves an NVIDIA GPU between its
 * two kernel drivers, and what of it this process holds
 * (src/backend/kms/kms_driver.c).
 */
#ifndef FNL_BACKEND_KMS_DRIVER_H
#define FNL_BACKEND_KMS_DRIVER_H

int  gpu_driver_list(const App *a, GpuDriver *out, int max);
bool gpu_driver_bind(const App *a, const GpuDriver *g, const char *driver);
bool gpu_driver_held(const App *a, const GpuDriver *g, bool every_nvidia,
					 char *what, size_t size);

#endif
