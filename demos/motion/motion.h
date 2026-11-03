/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * What the motion demo's files share. main.c is the program -- the clock
 * and the overlay -- and names no graphics API; scene_vulkan.c is the
 * device's half. F5's metrics panel is the library's.
 */

#ifndef MOTION_H
#define MOTION_H

#include <fenestal/fenestal.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the cube's draw is given. cube.vert declares the same block; the
 * asserts are what stop the two drifting apart. */
typedef struct {
	float    mvp[16];
	float    light[4];       /* towards the light, in the cube's own space */
	uint32_t srgb;           /* the scene fills it in: it knows the target */
} CubePush;

_Static_assert(sizeof(CubePush) == 84, "CubePush must match cube.vert");
_Static_assert(offsetof(CubePush, light) == 64, "CubePush layout drifted");
_Static_assert(offsetof(CubePush, srgb) == 80, "CubePush layout drifted");

/* ---- scene_vulkan.c ------------------------------------------------------ */

void scene_vulkan_on_gpu_created(void *ud, FnlApp *app);
void scene_vulkan_on_gpu_lost(void *ud, FnlApp *app);
/* From on_record: the cube, once, into the frame's scene pass. */
void scene_vulkan_draw(FnlApp *app, const FnlFrame *f, CubePush *push);

#endif /* MOTION_H */
