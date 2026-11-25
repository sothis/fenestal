/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The test pattern's star, as geometry rather than as arithmetic in a fragment
 * shader -- which is what gives FSAA something to do. Multisampling
 * antialiases *primitive coverage*, and a wedge computed from atan() in one
 * covering triangle has none to sample.
 *
 * Nothing is bound. 36 lit wedges, each two triangles, is 216 vertices built
 * from gl_VertexIndex, with the same ScenePush the background uses (the centre
 * and radius are derived from the scene's size, so the figure is the same at
 * every resolution).
 *
 * Two triangles per wedge rather than one: a wedge is 5 degrees wide, and a
 * single chord across it leaves the rim visibly flat -- 0.1% of the radius,
 * which is half a pixel at 4K. Split once, the error is a quarter of that.
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
} pc;

/* One value for the whole star, so it is converted to the surface's blend
 * space here, once a vertex, rather than once a fragment. */
layout(location = 0) flat out float v_shade;

const float PI = 3.14159265;
const int   WEDGES = 36;      /* lit wedges around the circle */
const int   SEGS   = 2;       /* triangles per wedge          */

/* The sRGB EOTF, as in scene.frag, and its inverse. */
float to_linear(float c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float to_srgb(float c)
{
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

void main()
{
	int tri  = gl_VertexIndex / 3;
	int vert = gl_VertexIndex % 3;
	int wedge = tri / SEGS;
	int seg   = tri % SEGS;

	/* Centred on and sized against the canvas, then back into this window's
	 * own pixels: a joined picture splits one star between two windows, and
	 * a window showing the whole canvas is the case origin (0, 0). */
	vec2  mid = pc.canvas * 0.5 - pc.origin;
	float R   = min(pc.canvas.x, pc.canvas.y) * 0.28;

	/* One wedge every 10 degrees, lit for the first 5 of them. */
	float step  = 2.0 * PI / float(WEDGES);
	float base  = float(wedge) * step - (pc.time + pc.time_bias) * 0.35;
	float span  = step * 0.5 / float(SEGS);
	float a0    = base + float(seg) * span;
	float a1    = a0 + span;

	/* Fan from the centre: vertex 0 is the hub, 1 and 2 the rim. */
	vec2 p = mid;
	if (vert == 1) p = mid + R * vec2(cos(a0), sin(a0));
	if (vert == 2) p = mid + R * vec2(cos(a1), sin(a1));

	/* The wedges take the window's tint exactly as scene.frag's pattern
	 * does: a multiplier on the *linear* value, encoded on the way out only
	 * when the surface will not. The tint is flat here, so the whole star is
	 * one colour -- hence one value, computed once a vertex. */
	float avg = (pc.tint.r + pc.tint.g + pc.tint.b) / 3.0;
	float lin = to_linear(0.82) * avg;
	v_shade   = pc.srgb != 0u ? lin : to_srgb(lin);
	/* The temporal accumulation's subpixel offset (FnlFrame::jitter_x): a
	 * uniform shift of the whole picture, which the resolve's `- jitter`
	 * undoes. NDC is 2 * pixels / size, and the scene is orthographic, so
	 * adding it to the clip-space xy is exactly the pixel shift. */
	vec2 j = 2.0 * pc.jitter / pc.size;
	gl_Position = vec4(p / pc.size * 2.0 - 1.0 + j, 0.0, 1.0);
}
