/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * One solid rectangle per instance: the corners come from gl_VertexIndex and
 * the rectangle is an instance-rate attribute, so every filled rectangle of a
 * frame is a single draw call.
 *
 * The attribute block must match RectInstance in app.h, which asserts its own
 * offsets.
 */

layout(push_constant) uniform Push {
	vec2 screen;          /* framebuffer size, pixels */
} pc;

layout(location = 0) in vec4 i_rect;    /* x, y, w, h -- pixels */
layout(location = 1) in vec4 i_color;

/* flat: one value for the whole primitive. */
layout(location = 0) flat out vec4 v_color;

void main()
{
	/* Strip order is (0,0) (1,0) (0,1) (1,1): the low two bits of the index,
	 * so four vertices and no array. */
	vec2 k = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);

	v_color = i_color;
	vec2 px = i_rect.xy + k * i_rect.zw;
	/* Pixel origin is top-left, matching Vulkan's NDC y-down. */
	gl_Position = vec4(px / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}
