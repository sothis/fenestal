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

layout(location = 0)      out vec2 v_px;     /* fragment position, pixels */
layout(location = 1) flat out vec4 v_p0p1;
layout(location = 2) flat out vec4 v_p2hf;
layout(location = 3) flat out vec4 v_color;

void main()
{
	/* Strip order is (0,0) (1,0) (0,1) (1,1): the low two bits of the index,
	 * so four vertices and no array. */
	vec2 k = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);

	vec2  p0 = i_p0p1.xy, p1 = i_p0p1.zw, p2 = i_p2hf.xy;
	float pad = i_p2hf.z + i_p2hf.w + 1.0;
	vec2  lo  = min(min(p0, p1), p2) - pad;
	vec2  hi  = max(max(p0, p1), p2) + pad;

	vec2 px = mix(lo, hi, k);
	v_px    = px;
	v_p0p1  = i_p0p1;
	v_p2hf  = i_p2hf;
	v_color = i_color;
	/* Pixel origin is top-left, matching Vulkan's NDC y-down. */
	gl_Position = vec4(px / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}
