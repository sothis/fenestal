/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * motion2: motion's lit cube, turning for as long as the window is open,
 * drawn with Direct3D 12 or Vulkan -- both on Windows, Vulkan alone
 * elsewhere, the library choosing between them. F5 shows the library's
 * metrics panel (hidden at start); Escape quits.
 *
 * motion's program with a scene per renderer instead of one. This file is
 * the program and names no graphics API: each scene file is one API's
 * (scene_d3d12.c, scene_vulkan.c), the build says which it compiled by
 * SCENE_HAS_<API>, and the table below picks the one the library
 * chose.
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
 *
 * As motion does, F2 turns on a *drift* of the whole picture as well as
 * the clock, which is what fnl_set_scene_motion_px() is for: a scene that
 * only spins has no motion for the library's temporal antialiasing to
 * reproject, and the drift is what that mode is looked at in. F3 switches
 * the mode itself and F4 pins the scene to half the window, so the drifting
 * picture can be seen with the accumulation on and off at the scale it is
 * for. The vector is computed from the pan every frame, never chosen by
 * hand, so it cannot describe a move the picture did not make, and the
 * projection carries FnlFrame::jitter_x as a temporal gather needs. F2 is
 * the library's own binding for the scene's clock, so this reads it back
 * with fnl_scene_motion() rather than ever seeing the key itself.
 */

#include "motion2.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE       "fenestal-demo-motion2"
#define HINT        "F2 drift   F3 TAAU   F4 half   F5 metrics   Esc quit"

#define PI_D        3.14159265358979
#define SPIN_Y      0.9                   /* radians a second, about y */
#define SPIN_X      0.55                  /* and about x */
#define CAM_DIST    2.6f                  /* camera to the cube's centre */
#define FOV_Y       (60.0f * (float)PI_D / 180.0f)
#define Z_NEAR      0.1f
#define Z_FAR       20.0f
#define MARGIN      12                    /* points from the content edge */
/* How far the camera swings sideways, in world units, and how often it makes
 * the there-and-back trip: a pan wide enough to sweep the cube across a
 * 960-point window and slow enough that the eye reads it as a camera move
 * rather than a stutter. The vector the resolve is given is derived from this
 * pan every frame and never chosen by hand, so the two cannot disagree. */
#define PAN_AMP     0.8f
#define DRIFT_HZ    0.15

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

#if !defined(SCENE_HAS_VULKAN) && !defined(SCENE_HAS_D3D12)
#error "no scene: the build defines SCENE_HAS_<API> for each one it compiles"
#endif

static const Scene SCENES[] = {
#ifdef SCENE_HAS_D3D12
	{ FNL_RENDERER_D3D12, scene_d3d12_on_gpu_created,
	  scene_d3d12_on_gpu_lost, scene_d3d12_draw },
#endif
#ifdef SCENE_HAS_VULKAN
	{ FNL_RENDERER_VULKAN, scene_vulkan_on_gpu_created,
	  scene_vulkan_on_gpu_lost, scene_vulkan_draw },
#endif
};
#define SCENE_COUNT (sizeof SCENES / sizeof SCENES[0])

/* The row drawing this device generation; NULL before the first and where
 * the renderer has none. Written in on_gpu_created, read on the render
 * thread, never two callbacks at once. */
static const Scene *g_scene;

/* Whether the scene is drifting, which is fnl_scene_motion()'s answer read
 * in on_overlay and the only thing that decides it: F2 is the library's
 * binding, and the scene's clock is the same state. */
static bool g_drift;

/* ---- the callbacks ------------------------------------------------------- */

/* The camera for this frame: swung on x by the pan, a function of the
 * scene's own clock -- which is stopped while F2 is off -- so the drift
 * begins from rest each time it is switched on rather than jumping in. A
 * scene's own turn moves each corner differently and no one vector can say
 * so; a camera's pan moves every pixel alike, which is what
 * fnl_set_scene_motion_px() describes. */
static Mat4 camera(float seconds)
{
	const float x = g_drift
		? PAN_AMP * (float)sin(2.0 * PI_D * DRIFT_HZ * (double)seconds)
		: 0.0f;
	return mat4_translate(x, 0.0f, -CAM_DIST);
}

/* Where the world's origin lands, in scene pixels, through `proj` and
 * `view`: the perspective divide and the viewport, no rotation, since the
 * camera's pan is the one thing that slides the whole picture. */
static void origin_px(const Mat4 *proj, const Mat4 *view, int w, int h,
					  float *x, float *y)
{
	const Mat4 mvp = mat4_mul(*proj, *view);
	const float cw = mvp.m[15];
	*x = (mvp.m[12] / cw * 0.5f + 0.5f) * (float)w;
	*y = (mvp.m[13] / cw * 0.5f + 0.5f) * (float)h;
}

/* How far the picture moved since the frame before it, in scene pixels: the
 * world origin's place through this frame's camera against the last frame's,
 * from the same camera the draw uses, so the vector cannot describe a move
 * the picture did not make. The origin sits at the cube's depth, which is
 * where the picture's content is; a perspective pan moves nearer and further
 * pixels by slightly different amounts, and one vector cannot say so -- which
 * is the clamp's work, not the vector's. A still camera, the default, needs
 * no vector. */
static void motion_update(FnlApp *app, const FnlFrame *f)
{
	static bool have;
	static float lx, ly;

	if (!g_drift || f->scene_w_px < 1 || f->scene_h_px < 1) {
		have = false;
		fnl_set_scene_motion_px(app, 0.0f, 0.0f);
		return;
	}
	const int w = f->scene_w_px, h = f->scene_h_px;
	const Mat4 proj = mat4_perspective(FOV_Y, (float)w / (float)h,
									   Z_NEAR, Z_FAR);
	const Mat4 view = camera(f->scene_time);
	float x, y;
	origin_px(&proj, &view, w, h, &x, &y);
	/* The first drifting frame has no frame before it to move from, so it
	 * moves nothing and seeds the pair. */
	if (!have) {
		lx = x;
		ly = y;
		have = true;
	}
	fnl_set_scene_motion_px(app, x - lx, y - ly);
	lx = x;
	ly = y;
}

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

	/* The picture's motion, and the vector the resolve reprojects by: from
	 * the frame's own clock and the same camera the scene is drawn with, so
	 * the vector matches the movement it describes. */
	motion_update(app, f);

	if (f->scene_w_px < 1 || f->scene_h_px < 1)
		return;

	const Mat4 rot = mat4_mul(mat4_rotate_y(turn(f->time, SPIN_Y)),
							  mat4_rotate_x(turn(f->time, SPIN_X)));
	Mat4 proj = mat4_perspective(
		FOV_Y, (float)f->scene_w_px / (float)f->scene_h_px, Z_NEAR, Z_FAR);
	/* The temporal accumulation's subpixel offset, which only the caller can
	 * put in its projection (FnlFrame::jitter_x): a uniform shift of the
	 * picture by that many scene pixels, which the shader's `- jitter` undoes
	 * exactly. A pixel-space shift in NDC is 2*jx/scene_w on x, applied to
	 * every vertex alike -- so it rides in column 2, which multiplies z, by
	 * the -z that clip.w is: clip.x += delta*w = delta*(-z). A constant added
	 * to clip.x instead would be scaled by the perspective divide and land
	 * only 1/depth of the way at the cube, a different and wrong offset. Zero
	 * unless FNL_AA_TAAU is in force and the scene moves, so a program that
	 * never asks for the mode draws exactly as before. */
	proj.m[8] -= 2.0f * f->jitter_x / (float)f->scene_w_px;
	proj.m[9] -= 2.0f * f->jitter_y / (float)f->scene_h_px;
	const Mat4 view = camera(f->scene_time);

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
	/* Drained every frame: a waiting event is itself a reason to record a
	 * frame, and F2's change is one this program has to see to start or stop
	 * moving the picture. */
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		if (ev.type == FNL_EVENT_KEY && ev.key.pressed &&
			!ev.key.repeat && ev.key.key == FNL_KEY_F2) {
			g_drift = fnl_scene_motion(app);
			if (!g_drift)
				fnl_set_scene_motion_px(app, 0.0f, 0.0f);
		}
		/* F3 toggles the temporal accumulation, which is what the drift is
		 * for: with the scene moving, the resolve gathers the subpixel
		 * samples the jitter spreads and reprojects the history by the
		 * vector. Off is the plain frame, the comparison to judge it
		 * against. */
		if (ev.type == FNL_EVENT_KEY && ev.key.pressed &&
			!ev.key.repeat && ev.key.key == FNL_KEY_F3)
			fnl_set_antialias(app,
							  fnl_antialias(app) == FNL_AA_TAAU
								  ? FNL_AA_NONE
								  : FNL_AA_TAAU);
		/* F4 pins the scene to half the window and back, so the fetch has an
		 * upscale to do: at 1:1 a subpixel gain is invisible, and this is the
		 * scale that shows what the accumulation is for. Whether a pin is in
		 * force is scene smaller than area, which is what the two calls
		 * answer -- a scene-size read alone gives the area itself when
		 * nothing is pinned. */
		if (ev.type == FNL_EVENT_KEY && ev.key.pressed &&
			!ev.key.repeat && ev.key.key == FNL_KEY_F4) {
			int sw, sh, aw, ah;
			fnl_scene_size_px(app, &sw, &sh);
			fnl_content_size_px(app, &aw, &ah);
			if (sw > 0 && sw < aw)
				fnl_set_scene_size_px(app, 0, 0);
			else if (aw > 0 && ah > 0)
				fnl_set_scene_size_px(app, aw / 2, ah / 2);
		}
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
