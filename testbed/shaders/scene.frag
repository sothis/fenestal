/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The window's scene -- one of two pictures, chosen by pc.content.
 *
 * The first is the test pattern, drawn at whatever resolution the scene is
 * being rendered at. A pattern rather than a triangle because the thing worth
 * seeing is the render scale (see DESIGN.md): the scene may be rendered
 * smaller than the window and stretched back over it.
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
 * The second is a scene of its own -- see other_scene() -- which a window
 * draws instead, so that two windows of one program show different content
 * and not one picture twice.
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
	uint  content;   /* 0: the pattern; nonzero: the other scene */
	/* x: what light authored against SDR white is in the target's unit
	 * (FnlOutputColor::sdr_scale); y: how far above SDR white the output
	 * goes (its peak); zw unused, and the vec4 keeps the block a multiple
	 * of 16 for std140. */
	vec4  colour;
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

/* ---- the second window's picture ------------------------------------------
 *
 * The demos' lit cube, turning in space. A ray is cast per pixel into a box
 * that spins, one colour a face, lit from one side. It shares the push block,
 * the jitter and the colour pipeline with the pattern and nothing else -- no
 * canvas, no star, no ramps -- because a window that draws it is showing
 * content of its own, not another arrangement of the first window's.
 */

/* One rotation, in the plane the two components name. */
vec2 turn(vec2 v, float a)
{
	float c = cos(a), s = sin(a);
	return vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

vec3 other_scene(vec2 p, vec2 size, float t)
{
	/* The pixel as a ray: y up, since pixels are y-down, and x stretched so
	 * the cube is round whatever the window's aspect; the eye sits back
	 * along +z looking down -z. */
	vec2 q = (p / max(size, vec2(1.0))) * 2.0 - 1.0;
	q.y    = -q.y;
	q.x   *= size.x / max(size.y, 1.0);
	vec3 ro = vec3(0.0, 0.0, 3.0);
	vec3 rd = normalize(vec3(q * 1.1, -2.2));

	/* The cube turns about the vertical and, slower, about x. Asked in its
	 * own frame, so the box below is the axis-aligned one whose slabs are;
	 * the ray is turned the other way instead. */
	float a = t * 0.60;
	float b = t * 0.35;
	vec3 o = ro, d = rd;
	o.xz = turn(o.xz, -a);
	d.xz = turn(d.xz, -a);
	o.yz = turn(o.yz, -b);
	d.yz = turn(d.yz, -b);

	/* The background: a dark gradient, a little warmer towards the bottom. */
	vec3 c = mix(lin(vec3(0.03, 0.04, 0.08)), lin(vec3(0.10, 0.09, 0.16)),
				 p.y / max(size.y, 1.0));

	/* The slabs. 1/d with d kept off zero, the sign carried out rather than
	 * in, so a ray along an axis yields a large finite t and not an infinity
	 * the drivers are free to make of nothing. Zero counts as positive:
	 * sign() would give it a zero m and collapse that axis's slab to a point,
	 * which the centre column of an untilted frame would then miss. */
	float H = 0.72;
	vec3  s = mix(vec3(-1.0), vec3(1.0), step(0.0, d));
	vec3  m = s / max(abs(d), vec3(1e-5));
	vec3  ta = (vec3(-H) - o) * m;
	vec3  tb = (vec3(H) - o) * m;
	vec3  tn = min(ta, tb);
	vec3  tf = max(ta, tb);
	float t_near = max(max(tn.x, tn.y), tn.z);
	float t_far  = min(min(tf.x, tf.y), tf.z);

	if (t_far > 0.0 && t_near <= t_far) {
		t_near = max(t_near, 0.0);
		vec3 hit = o + t_near * d;
		/* The face entered: the axis whose near crossing came last. */
		vec3 nrm;
		if (tn.x >= tn.y && tn.x >= tn.z)
			nrm = vec3(hit.x > 0.0 ? 1.0 : -1.0, 0.0, 0.0);
		else if (tn.y >= tn.z)
			nrm = vec3(0.0, hit.y > 0.0 ? 1.0 : -1.0, 0.0);
		else
			nrm = vec3(0.0, 0.0, hit.z > 0.0 ? 1.0 : -1.0);

		/* The demo's colours, a face each, lit in the cube's own frame as
		 * the demo lights them: the light turns with the box. */
		vec3 face = nrm.x > 0.5  ? vec3(0.92, 0.33, 0.28)
				  : nrm.x < -0.5 ? vec3(0.20, 0.72, 0.80)
				  : nrm.y > 0.5  ? vec3(0.40, 0.80, 0.35)
				  : nrm.y < -0.5 ? vec3(0.78, 0.36, 0.72)
				  : nrm.z > 0.5  ? vec3(0.28, 0.47, 0.90)
								 : vec3(0.95, 0.80, 0.30);
		float diff = max(dot(nrm, normalize(vec3(0.4, 0.6, 0.7))), 0.0);
		c = lin(face) * (0.18 + 0.82 * diff);
	}
	return c;
}

const float PI = 3.14159265;

void main()
{
	/* The second window's own picture, filling its own rectangle: no
	 * canvas, no tint and no star -- a scene of its own, not the pattern's
	 * arrangement. The jitter still moves it, applied in the vertex stage,
	 * as it moves anything else. */
	if (pc.content != 0u) {
		const vec3 c = other_scene(v_uv * pc.size, pc.size, pc.time) *
					   pc.colour.x;
		out_color = vec4(pc.srgb != 0u ? c : enc(c), 1.0);
		return;
	}

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
			/* An HDR output's ramp climbs over SDR white for its last
			 * quarter, to as far as the output goes: the one patch on the
			 * pattern that an SDR one cannot show, and so what tells the
			 * two apart in a capture. */
			if (pc.srgb != 0u && pc.colour.y > 1.0)
				c *= 1.0 + (pc.colour.y - 1.0) * smoothstep(0.75, 1.0, t);
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

	/* Into the target's unit for SDR white, which is 1.0 on every output
	 * but a scRGB one (FnlOutputColor::sdr_scale). */
	c *= pc.colour.x;

	/* Linearised unconditionally, above, then encoded on the way out unless
	 * the hardware is going to. The conditional is on the *output*, not the
	 * input: a sampler on an _SRGB image returns linear whatever the
	 * swapchain is, so a texture() joining this composite lands in the same
	 * light as the generated colour on both paths. Nothing here samples yet,
	 * so today it is a no-op (see DESIGN *Colour*). */
	out_color = vec4(pc.srgb != 0u ? c : enc(c), 1.0);
}
