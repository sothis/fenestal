/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * Cover the bounding box of one thick shape -- a segment, two meeting at a
 * bend, or a closed rectangle (which shape is which is p2's business; see
 * shape.frag). Taking the extent of all three points covers every one of them,
 * since a rectangle arrives as its two opposite corners.
 *
 * The corners come from gl_VertexIndex and the stroke is instance-rate, so
 * every stroke of a frame is one draw call. The box is the stroke's extent
 * grown by its radius plus the antialiasing falloff, so no coverage is clipped.
 *
 * The attribute block must match ShapeInstance in app.h, which asserts its own
 * offsets.
 */

layout(push_constant) uniform Push {
	vec2 screen;      /* framebuffer size, pixels */
} pc;

layout(location = 0) in vec4 i_p0p1;   /* p0.xy, p1.xy -- pixels          */
layout(location = 1) in vec4 i_p2hf;   /* p2.xy, half_w, feather          */
layout(location = 2) in vec4 i_color;
layout(location = 3) in vec4 i_color2; /* gradient's second colour, and  */
layout(location = 4) in vec4 i_kind;   /* the ShapeKind, in .x             */

layout(location = 0)      out vec2 v_px;     /* fragment position, pixels */
layout(location = 1) flat out vec4 v_p0p1;
layout(location = 2) flat out vec4 v_p2hf;
layout(location = 3) flat out vec4 v_color;
layout(location = 4) flat out vec4 v_color2;
layout(location = 5) flat out float v_kind;

void main()
{
	/* Strip order is (0,0) (1,0) (0,1) (1,1): the low two bits of the index,
	 * so four vertices and no array. */
	vec2 k = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);

	vec2  p0 = i_p0p1.xy, p1 = i_p0p1.zw, p2 = i_p2hf.xy;
	/* The gradient's half_w is an angle in degrees, not a radius: its quad
	 * is the rectangle itself, so the fragment shader's ramp is all that is
	 * drawn. A stroke's box is grown by its radius plus the antialiasing
	 * falloff, once more for the ramp's own half-width, so no coverage is
	 * clipped at the box's border. */
	float pad = (i_kind.x > 2.5 && i_kind.x < 3.5)
		? 0.0 : i_p2hf.z + i_p2hf.w + 1.0;
	vec2  lo  = min(min(p0, p1), p2) - pad;
	vec2  hi  = max(max(p0, p1), p2) + pad;

	vec2 px = mix(lo, hi, k);
	v_px     = px;
	v_p0p1   = i_p0p1;
	v_p2hf   = i_p2hf;
	if (i_kind.x > 2.5 && i_kind.x < 3.5) {
		/* The gradient's axis, the corner it enters by and the reciprocal of
		 * its extent depend on the instance alone: worked out here, four
		 * times a box, and not at every pixel of it. The extent is not the
		 * diagonal's projection -- that is right only where the two
		 * components share a sign, and is zero on a square at 45 degrees --
		 * and a box with no extent along the axis has no ramp to run, so
		 * its reciprocal is 0 and t stays 0, the first colour. */
		float a = radians(i_p2hf.z);
		vec2  dir = vec2(cos(a), sin(a));
		vec2  lo = i_p0p1.xy, hi = i_p0p1.zw;
		float ext = abs(hi.x - lo.x) * abs(dir.x) +
					abs(hi.y - lo.y) * abs(dir.y);
		v_p0p1 = vec4(dir.x >= 0.0 ? lo.x : hi.x, dir.y >= 0.0 ? lo.y : hi.y,
					  dir);
		v_p2hf = vec4(ext > 1e-6 ? 1.0 / ext : 0.0, 0.0, 0.0, 0.0);
	}
	v_color  = i_color;
	v_color2 = i_color2;
	v_kind   = i_kind.x;
	/* Pixel origin is top-left, matching Vulkan's NDC y-down. */
	gl_Position = vec4(px / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}
