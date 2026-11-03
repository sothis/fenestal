/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * What texture3's files share. main.c is the program -- the clock, the
 * overlay and the texture's pixels -- and names no graphics API; each scene
 * file is the device's half for one API: scene_metal.m and scene_vulkan.c.
 * F5's metrics panel is the library's.
 */

#ifndef TEXTURE3_H
#define TEXTURE3_H

#include <fenestal/fenestal.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the cube's draw is given. The shaders declare the same block; the
 * asserts are what stop them drifting apart. Vulkan pushes it, and Metal
 * hands it to the encoder as bytes, padded to MSL's 96 (scene_metal.m). */
typedef struct {
	float    mvp[16];
	float    light[4];       /* towards the light, in the cube's own space */
	uint32_t srgb;           /* the scene fills it in: it knows the target */
} CubePush;

_Static_assert(sizeof(CubePush) == 84, "CubePush must match cube.vert");
_Static_assert(offsetof(CubePush, light) == 64, "CubePush layout drifted");
_Static_assert(offsetof(CubePush, srgb) == 80, "CubePush layout drifted");

/* ---- the texture: main.c --------------------------------------------------
 *
 * Made at start rather than loaded, so the demo ships no file: TEX_SIZE
 * square, RGBA, as sRGB bytes -- the numbers a colour picker gives, which
 * each scene uploads as an sRGB image so sampling returns linear. */
#define TEX_SIZE 128

void texture_pixels(unsigned char *rgba);

/* ---- scene_metal.m -------------------------------------------------------
 *
 * Objective-C, which Metal needs; called from main.c's C through these
 * prototypes, which name no Metal type. */

void scene_metal_on_gpu_created(void *ud, FnlApp *app);
void scene_metal_on_gpu_lost(void *ud, FnlApp *app);
/* From on_record: the cube, once, into the frame's scene encoder. */
void scene_metal_draw(FnlApp *app, const FnlFrame *f, CubePush *push);

/* ---- scene_vulkan.c ------------------------------------------------------ */

void scene_vulkan_on_gpu_created(void *ud, FnlApp *app);
void scene_vulkan_on_gpu_lost(void *ud, FnlApp *app);
/* From on_record: the cube, once, into the frame's scene pass. */
void scene_vulkan_draw(FnlApp *app, const FnlFrame *f, CubePush *push);

#endif /* TEXTURE3_H */
