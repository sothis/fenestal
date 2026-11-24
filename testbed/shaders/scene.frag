/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The scene: a test pattern, drawn at whatever resolution the scene is being
 * rendered at. A pattern rather than a triangle because the thing worth seeing
 * is the render scale (see DESIGN.md): the scene may be rendered smaller than
 * the window and stretched back over it.
 *
 * Everything is laid out in *scene pixels* -- pc.size is the scene's own
 * resolution -- so a one-pixel grid line lands on screen as whatever the
 * upscale makes of one pixel.
 *
 *   grid       one-pixel lines every 32, brighter every 128 -- the pixel grid
 *              itself, and the clearest read on a filter: crisp under 1:1,
 *              soft and uneven under a non-integer upscale
 *   star       36 wedges meeting at a point, which is where a resampler's
 *              failures gather -- moire near the centre, ragged edges outward
 *   ramps      a continuous grey ramp with a stepped one under it (banding and
 *              transfer function), then the six primaries at full and half
 *   border     two pixels at the very edge, so the scene's own rectangle is
 *              visible: letterboxing shows as a gap between it and the window
 *
 * Colours are authored in sRGB, linearised on the way into the composite, and
 * encoded on the way out only when the surface will not do it. See the note at
 * out_color, and DESIGN *Colour*.
 */

layout(push_constant) uniform Push {
	vec2  size;      /* this window's scene resolution, px */
	vec2  jitter;    /* the temporal accumulation's subpixel offset, px */
	vec2  canvas;    /* the pattern's whole canvas, px */
	vec2  origin;    /* this window's corner inside that canvas, px */
	vec4  tint;      /* linear-light multiplier: the window's own scene */
	float time;      /* seconds; frozen at 0 when motion is off */
	float time_bias; /* this window's own phase, for its own scene */
	uint  srgb;      /* nonzero when the surface blends in linear light */
	float pad;
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

/* The sRGB EOTF, matching srgb_to_linear() in src/core/app.h, and its
 * inverse. */
float to_linear(float c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float to_srgb(float c)
{
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

vec3 lin(vec3 c)
{
	return vec3(to_linear(c.r), to_linear(c.g), to_linear(c.b));
}
vec3 enc(vec3 c)
{
	return vec3(to_srgb(c.r), to_srgb(c.g), to_srgb(c.b));
}

const float PI = 3.14159265;

void main()
{
	/* The pattern is laid out on the canvas, and this window shows the part
	 * of it that starts at `origin`: a window showing the whole canvas is
	 * one at (0, 0), and several windows each showing a piece of one picture
	 * are the same pattern at different origins -- a join. The canvas is
	 * also what the star and the ramps are sized against, so a shared canvas
	 * splits one star across two windows rather than drawing one each. */
	vec2  p   = v_uv * pc.size + pc.origin;  /* canvas pixels, top-left */
	vec2  mid = pc.canvas * 0.5;
	float minside = min(pc.canvas.x, pc.canvas.y);
	const float t_now = pc.time + pc.time_bias;

	/* Linear from here: every colour is linearised where it is chosen and
	 * not once at the end, because nearly all of them are constants, and a
	 * constant's pow() is folded by the driver's compiler. Once at the end
	 * is three pow() a pixel on the colour the branches merged into, which
	 * on a software rasteriser is most of the frame. The ramps are the
	 * only colours computed per pixel, and are one grey each. */
	vec3 c = lin(vec3(0.055));           /* the field the rest is drawn on */

	/* Pixel grid. Whole-pixel tests, no antialiasing: a line that is exactly
	 * one scene pixel wide is the measurement, and softening it here would hide
	 * what the upscale does to it. */
	vec2 g32  = mod(p, 32.0);
	vec2 g128 = mod(p, 128.0);
	if (g32.x < 1.0 || g32.y < 1.0)
		c = lin(vec3(0.18));
	if (g128.x < 1.0 || g128.y < 1.0)
		c = lin(vec3(0.34));

	/* The star's field: the disc the wedges are drawn over, its hub and its
	 * rim. The wedges themselves are geometry (see
	 * testbed/shaders/star.vert), so that FSAA has something to
	 * sample.
	 *
	 * The hub is flat: at the very centre the wedges are thinner than a pixel
	 * and would alias into noise whatever any filter does. */
	vec2  d = p - mid;
	float r = length(d);
	float R = minside * 0.28;
	if (r < R) {
		c = r < minside * 0.02 ? lin(vec3(0.55)) : lin(vec3(0.10));
		/* One ring at the rim, to read the circle's edge against the grid. */
		if (r > R - 1.5)
			c = lin(vec3(0.35, 0.85, 0.45));
	}

	/* Ramps along the bottom: continuous grey, stepped grey, then primaries.
	 * Sized in scene pixels so they stay one band thick however small the
	 * scene is, and skipped outright when there is no room for them. */
	float band = max(floor(pc.canvas.y * 0.045), 4.0);
	float base = pc.canvas.y - 3.0 * band - 2.0;
	if (pc.canvas.y > 12.0 * band && p.y >= base && p.y < base + 3.0 * band) {
		float t = clamp(p.x / max(pc.canvas.x - 1.0, 1.0), 0.0, 1.0);
		if (p.y < base + band) {
			c = vec3(to_linear(t));                       /* continuous */
		} else if (p.y < base + 2.0 * band) {
			c = vec3(to_linear(floor(t * 16.0) / 15.0));  /* 16 steps    */
		} else {
			int  i  = int(clamp(t * 6.0, 0.0, 5.999));    /* six primaries */
			/* Each channel is 0 or 1, which linearise to themselves, so
			 * the half-intensity bar is the full one times half's linear
			 * value. */
			vec3 bar = i == 0 ? vec3(1.0, 0.0, 0.0)
					 : i == 1 ? vec3(1.0, 1.0, 0.0)
					 : i == 2 ? vec3(0.0, 1.0, 0.0)
					 : i == 3 ? vec3(0.0, 1.0, 1.0)
					 : i == 4 ? vec3(0.0, 0.0, 1.0)
							  : vec3(1.0, 0.0, 1.0);
			/* Half intensity in the lower half of the strip: the same hue at
			 * two levels is what shows a transfer function going wrong. */
			c = p.y < base + 2.5 * band ? bar : bar * to_linear(0.5);
		}
	}

	/* A bar sweeping across, for tearing and pacing -- the one thing a still
	 * frame cannot show. Gated on the *clock* (pc.time) and not this
	 * window's phase, so a window with a time_bias still rests when the
	 * scene's motion is off: the bias moves the star, not a still window's
	 * recording. Sized against the canvas so a joined picture passes it
	 * from one window to the next. */
	float sweep = fract(t_now * 0.25) * pc.canvas.x;
	if (pc.time > 0.0 && abs(p.x - sweep) < max(pc.canvas.x * 0.004, 2.0))
		c = lin(vec3(0.95, 0.75, 0.25));

	/* The scene's own edge, two pixels of it, on the *window's* rectangle
	 * and not the canvas's: under letterboxing this is the line the black
	 * bars start at, and under a join it is where one window's piece ends.
	 * Drawn in the window's own coordinates, so every window frames itself
	 * whole whatever part of the canvas it shows. */
	vec2 q = v_uv * pc.size;
	if (q.x < 2.0 || q.y < 2.0 || q.x >= pc.size.x - 2.0 ||
		q.y >= pc.size.y - 2.0)
		c = lin(vec3(0.35, 0.85, 0.45));

	/* The window's own scene: a tint, a phase and a canvas of its own, so a
	 * second window can draw a picture that is plainly not the first's --
	 * a mirror is the default (tint 1, no bias, the same canvas), a joined
	 * picture shares the canvas and differs in origin, and an own scene
	 * differs in all three. */
	c *= pc.tint.rgb;

	/* Linearised unconditionally, above, then encoded on the way out unless
	 * the hardware is going to. The conditional is on the *output*, not the
	 * input: a sampler on an _SRGB image returns linear whatever the
	 * swapchain is, so a texture() joining this composite lands in the same
	 * light as the generated colour on both paths. Nothing here samples yet,
	 * so today it is a no-op (see DESIGN *Colour*). */
	out_color = vec4(pc.srgb != 0u ? c : enc(c), 1.0);
}
