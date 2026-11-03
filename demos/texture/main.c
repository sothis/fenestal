/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * texture: a textured, lit cube, turning for as long as the window is open,
 * drawn with OpenGL or Vulkan -- both on Linux, and on a Mac (Vulkan on
 * MoltenVK, OpenGL 4.1), the library choosing between them; Vulkan alone
 * on Windows, which has no OpenGL renderer. F5 shows the library's metrics
 * panel (hidden at start); Escape quits.
 *
 * This file is the program and names no graphics API: each scene file is
 * one API's (scene_gl.c, scene_vulkan.c), the build says which it compiled
 * by SCENE_HAS_<API>, and the table below picks the one the library chose.
 * The texture's pixels are made here, and each scene uploads them its
 * own way.
 *
 * Two things here are the answers a moving scene on Fenestal has to arrive
 * at:
 *
 *   it asks for every frame  the library draws a frame again only when
 *                            something it can see has moved, and it cannot
 *                            see the cube turn. So on_record asks for the
 *                            next frame each time (fnl_invalidate()).
 *   it moves by the clock    the angle is a function of FnlFrame::time, not
 *                            a step a call, so the cube turns at the same
 *                            rate at any frame rate.
 */

#include "texture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE       "fenestal-demo-texture"
#define HINT        "F5 metrics   Esc quit"

#define PI_D        3.14159265358979
#define SPIN_Y      0.9                   /* radians a second, about y */
#define SPIN_X      0.55                  /* and about x */
#define CAM_DIST    2.6f                  /* camera to the cube's centre */
#define FOV_Y       (60.0f * (float)PI_D / 180.0f)
#define Z_NEAR      0.1f
#define Z_FAR       20.0f
#define MARGIN      12                    /* points from the content edge */

/* Towards the light, in the world: up, right and towards the camera.
 * Normalised by hand. */
static const float LIGHT[3] = { 0.42f, 0.80f, 0.43f };

static const FnlColor HINT_INK = { 0.55f, 0.58f, 0.65f, 1.0f };

/* ---- a little linear algebra ----------------------------------------------
 *
 * Column-major, the layout GLSL reads a mat4 in, so a matrix is pushed as
 * the sixteen floats it already is. */
typedef struct { float m[16]; } Mat4;

static Mat4 mat4_identity(void)
{
	Mat4 r = { { 0 } };
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

static Mat4 mat4_mul(Mat4 a, Mat4 b)
{
	Mat4 r = { { 0 } };
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += a.m[k * 4 + row] * b.m[c * 4 + k];
			r.m[c * 4 + row] = s;
		}
	return r;
}

static Mat4 mat4_translate(float x, float y, float z)
{
	Mat4 r = mat4_identity();
	r.m[12] = x;
	r.m[13] = y;
	r.m[14] = z;
	return r;
}

static Mat4 mat4_rotate_x(float a)
{
	Mat4 r = mat4_identity();
	const float c = cosf(a), s = sinf(a);
	r.m[5] = c;   r.m[6] = s;
	r.m[9] = -s;  r.m[10] = c;
	return r;
}

static Mat4 mat4_rotate_y(float a)
{
	Mat4 r = mat4_identity();
	const float c = cosf(a), s = sinf(a);
	r.m[0] = c;   r.m[2] = -s;
	r.m[8] = s;   r.m[10] = c;
	return r;
}

/* Vulkan's clip space: depth runs 0 to 1 and y points down, which is why
 * m[5] is negated. */
static Mat4 mat4_perspective(float fovy, float aspect, float zn, float zf)
{
	Mat4 r = { { 0 } };
	const float f = 1.0f / tanf(fovy * 0.5f);
	r.m[0]  = f / aspect;
	r.m[5]  = -f;
	r.m[10] = zf / (zn - zf);
	r.m[11] = -1.0f;
	r.m[14] = zn * zf / (zn - zf);
	return r;
}

/* An angle that turns at `rate` radians a second, kept inside one turn in
 * double first: a float of hours' worth of radians has lost the fraction
 * the cube's position is in. */
static float turn(double seconds, double rate)
{
	return (float)fmod(seconds * rate, 2.0 * PI_D);
}

/* ---- the texture -----------------------------------------------------------
 *
 * A checkerboard in a frame, so each face's edges and its orientation show
 * as the cube turns: TEX_CELLS cells a side in two colours, inside a border
 * TEX_BORDER texels deep, and one corner cell marked, the top left. */
#define TEX_CELLS  8
#define TEX_BORDER 6

static const unsigned char TEX_LIGHT[3]  = { 236, 232, 220 };
static const unsigned char TEX_DARK[3]   = { 52, 108, 196 };
static const unsigned char TEX_FRAME[3]  = { 32, 36, 44 };
static const unsigned char TEX_MARK[3]   = { 232, 96, 64 };

void texture_pixels(unsigned char *rgba)
{
	const int cell = TEX_SIZE / TEX_CELLS;
	for (int y = 0; y < TEX_SIZE; y++)
		for (int x = 0; x < TEX_SIZE; x++) {
			const int cx = x / cell, cy = y / cell;
			const bool frame = x < TEX_BORDER || y < TEX_BORDER ||
							   x >= TEX_SIZE - TEX_BORDER ||
							   y >= TEX_SIZE - TEX_BORDER;
			const unsigned char *c = frame               ? TEX_FRAME
								   : cx == 0 && cy == 0 ? TEX_MARK
								   : (cx + cy) & 1      ? TEX_DARK
														: TEX_LIGHT;
			unsigned char *p = &rgba[(y * TEX_SIZE + x) * 4];
			memcpy(p, c, 3);
			p[3] = 255;
		}
}

/* ---- which scene -----------------------------------------------------------
 *
 * A row for each scene file the build compiled. main() tells the library
 * these are the renderers there is a scene for; the library chooses which
 * draws -- the fastest it has measured on this machine, or its own order
 * before it has -- and on_gpu_created looks up the row fnl_renderer() names,
 * the first moment that is known, and again after a move to another
 * renderer. */
typedef struct {
	FnlRenderer renderer;
	void (*created)(void *ud, FnlApp *app);
	void (*lost)(void *ud, FnlApp *app);
	void (*draw)(FnlApp *app, const FnlFrame *f, CubePush *push);
} Scene;

#if !defined(SCENE_HAS_VULKAN) && !defined(SCENE_HAS_GL)
#error "no scene: the build defines SCENE_HAS_<API> for each one it compiles"
#endif

static const Scene SCENES[] = {
#ifdef SCENE_HAS_VULKAN
	{ FNL_RENDERER_VULKAN, scene_vulkan_on_gpu_created,
	  scene_vulkan_on_gpu_lost, scene_vulkan_draw },
#endif
#ifdef SCENE_HAS_GL
	{ FNL_RENDERER_OPENGL, scene_gl_on_gpu_created, scene_gl_on_gpu_lost,
	  scene_gl_draw },
#endif
};
#define SCENE_COUNT (sizeof SCENES / sizeof SCENES[0])

/* The row drawing this device generation; NULL before the first and where
 * the renderer has none. Written in on_gpu_created, read on the render
 * thread, never two callbacks at once. */
static const Scene *g_scene;

/* ---- the callbacks ------------------------------------------------------- */

static void on_gpu_created(void *ud, FnlApp *app)
{
	const FnlRenderer r = fnl_renderer(app);
	g_scene = NULL;
	for (size_t i = 0; i < SCENE_COUNT; i++)
		if (SCENES[i].renderer == r)
			g_scene = &SCENES[i];
	if (g_scene)
		g_scene->created(ud, app);
}

static void on_gpu_lost(void *ud, FnlApp *app)
{
	if (g_scene)
		g_scene->lost(ud, app);
}

static void on_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	(void)ud;

	/* Before anything can return: the cube is always turning, so the next
	 * frame always differs from this one. */
	fnl_invalidate(app);

	if (f->scene_w_px < 1 || f->scene_h_px < 1)
		return;

	const Mat4 rot = mat4_mul(mat4_rotate_y(turn(f->time, SPIN_Y)),
							  mat4_rotate_x(turn(f->time, SPIN_X)));
	const Mat4 proj = mat4_perspective(
		FOV_Y, (float)f->scene_w_px / (float)f->scene_h_px, Z_NEAR, Z_FAR);
	const Mat4 view = mat4_translate(0.0f, 0.0f, -CAM_DIST);

	CubePush push;
	memset(&push, 0, sizeof push);
	const Mat4 mvp = mat4_mul(proj, mat4_mul(view, rot));
	memcpy(push.mvp, mvp.m, sizeof push.mvp);
	/* The light through the rotation's inverse, which for a rotation is its
	 * transpose: each component is a column dotted with it. */
	for (int k = 0; k < 3; k++)
		push.light[k] = rot.m[k * 4 + 0] * LIGHT[0] +
						rot.m[k * 4 + 1] * LIGHT[1] +
						rot.m[k * 4 + 2] * LIGHT[2];
	if (g_scene)
		g_scene->draw(app, f, &push);
}

/* A one-line hint at the content's bottom left, where the window has room
 * for it. The metrics panel is the library's own: F5 is its key until a
 * program binds another (fnl_set_metrics_key()), and it draws itself. */
static void on_overlay(void *ud, FnlApp *app, const FnlFrame *f)
{
	(void)ud;
	(void)f;
	/* Drained every frame, though nothing here is acted on: a waiting
	 * event is itself a reason to record a frame. */
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
	}

	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	const int line = fnl_text_line(FNL_TEXT_BODY);
	const int hy = cy + ch - MARGIN - line;
	if (fnl_text_width(HINT, FNL_TEXT_BODY) + 2 * MARGIN <= cw &&
		hy >= cy + MARGIN)
		fnl_overlay_text(app, cx + MARGIN, hy, FNL_TEXT_BODY, HINT_INK, HINT);
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size           = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title          = TITLE;
	desc.width_pt       = 960;
	desc.height_pt      = 600;
	/* The renderers there is a scene for, and no others: none of them
	 * working is no window at all (FNL_ERR_UNSUPPORTED) rather than a
	 * fallback the scenes cannot draw with. */
	for (size_t i = 0; i < SCENE_COUNT; i++)
		desc.renderers |= FNL_RENDERER_BIT(SCENES[i].renderer);

	FnlApp *app;
	FnlResult rc = fnl_init(&desc, &app);
	if (rc < 0) {
		fprintf(stderr, "fnl_init: %s: %s\n", fnl_result_str(rc),
				fnl_last_error());
		return EXIT_FAILURE;
	}

	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size           = sizeof cb;
	cb.on_gpu_created = on_gpu_created;
	cb.on_gpu_lost    = on_gpu_lost;
	cb.on_record      = on_record;
	cb.on_overlay     = on_overlay;
	rc = fnl_run(app, &cb);
	if (rc < 0)
		fprintf(stderr, "fnl_run: %s: %s\n", fnl_result_str(rc),
				fnl_last_error());
	fnl_shutdown(app);
	return rc < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
