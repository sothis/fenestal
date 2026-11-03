/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * What the texture demo's files share. main.c is the program -- the clock,
 * the overlay and the texture's pixels -- and names no graphics API; each
 * scene file is the device's half for one API: scene_gl.c and
 * scene_vulkan.c. F5's metrics panel is the library's.
 */

#ifndef TEXTURE_H
#define TEXTURE_H

#include <fenestal/fenestal.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the cube's draw is given. The shaders declare the same block; the
 * asserts are what stop them drifting apart. Vulkan pushes it, and OpenGL
 * uploads it as a std140 uniform block, whose offsets for these three
 * members are the same. */
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

/* ---- scene_gl.c ---------------------------------------------------------- */

void scene_gl_on_gpu_created(void *ud, FnlApp *app);
void scene_gl_on_gpu_lost(void *ud, FnlApp *app);
/* From on_record: the cube, once, into the frame's framebuffer. */
void scene_gl_draw(FnlApp *app, const FnlFrame *f, CubePush *push);

/* ---- scene_vulkan.c ------------------------------------------------------ */

void scene_vulkan_on_gpu_created(void *ud, FnlApp *app);
void scene_vulkan_on_gpu_lost(void *ud, FnlApp *app);
/* From on_record: the cube, once, into the frame's scene pass. */
void scene_vulkan_draw(FnlApp *app, const FnlFrame *f, CubePush *push);

#endif /* TEXTURE_H */
