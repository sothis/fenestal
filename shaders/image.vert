/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * One picture, placed: a quad at the caller's rectangle. Its corners come
 * from gl_VertexIndex and the rectangle is a push constant, so an image
 * needs neither a vertex buffer nor an instance buffer -- one draw a call.
 *
 * The corner is the uv, so the image's row 0 is the rectangle's first row
 * (rect.y), which is how the cover and the glyph atlas read theirs.
 *
 * Two triangles rather than the covering triangle (shaders/fullscreen.vert):
 * the covering triangle's uv runs past the unit square outside the
 * rectangle, and the sampler would smear its clamped edge across whatever
 * the rectangle does not cover. A scissor would stop that but would have to
 * be saved and put back around the draw.
 *
 * The attribute block must match ImagePush in src/renderer/vulkan/
 * vk_internal.h, which asserts its offsets.
 */

layout(push_constant) uniform Push {
	vec4 rect;            /* x, y, w, h -- pixels */
	vec2 screen;          /* framebuffer size, pixels */
} pc;

layout(location = 0) out vec2 v_uv;

void main()
{
	const vec2 corner[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0),
								   vec2(0.0, 1.0), vec2(0.0, 1.0),
								   vec2(1.0, 0.0), vec2(1.0, 1.0));
	vec2 k = corner[gl_VertexIndex];

	v_uv = k;
	vec2 px = pc.rect.xy + k * pc.rect.zw;
	gl_Position = vec4(px / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}
