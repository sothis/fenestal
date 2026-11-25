/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The covering triangle the test pattern is drawn with: uv (0,0), (2,0), (0,2)
 * from gl_VertexIndex, y-down, placed by the viewport.
 *
 * Duplicated from the library's own shaders/fullscreen.vert rather than shared
 * with it: sharing would mean the library publishing a shader, and promising
 * its interface across versions in a language whose linkage is by location
 * number.
 */

layout(location = 0) out vec2 v_uv;

/* The same block scene.frag declares, so this stage can carry the temporal
 * accumulation's subpixel offset and the canvas this window shows a part of.
 * Declared whole though only `size` and `jitter` are read here (every stage
 * of one program declares the block as it is), and the fields the fragment
 * stage uses are simply not touched. */
layout(push_constant) uniform Push {
	vec2  size;       /* this window's scene resolution, px */
	vec2  jitter;     /* the temporal accumulation's subpixel offset, px */
	vec2  canvas;     /* the pattern's whole canvas, px */
	vec2  origin;     /* this window's corner inside that canvas, px */
	vec4  tint;       /* linear-light multiplier: the window's own scene */
	float time;       /* seconds; frozen at 0 when motion is off */
	float time_bias;  /* this window's own phase, for its own scene */
	uint  srgb;       /* nonzero when the surface blends in linear light */
	uint  content;    /* 0: the pattern; nonzero: the other scene */
} pc;

void main()
{
	v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	/* The whole picture shifts by the jitter, background and star alike, so
	 * the resolve undoes one shift and finds the samples it gathered. The
	 * covering triangle is oversized (uv to 2), so the shift only slides the
	 * pattern under it; NDC is 2 * pixels / size, orthographically. */
	vec2 j = 2.0 * pc.jitter / pc.size;
	gl_Position = vec4(v_uv * 2.0 - 1.0 + j, 0.0, 1.0);
}
