/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * One glyph per instance: the corners come from gl_VertexIndex and everything
 * per glyph is an instance-rate attribute, so all the overlay's text is a
 * single draw call.
 *
 * The attribute block must match GlyphInstance in app.h, which asserts its own
 * offsets.
 */

/* Only `screen` is read here; the other two are the fragment shader's.
 * Must match TextPush in src/renderer/vulkan/vk_internal.h, which asserts
 * its size. */
layout(push_constant) uniform Push {
	vec2  screen;         /* framebuffer size, pixels */
	uint  srgb;           /* fragment stage: the surface's blend space */
	float texel_u;        /* fragment stage: one atlas texel, normalised */
} pc;

layout(location = 0) in vec4 i_rect;    /* x, y, w, h -- pixels          */
layout(location = 1) in vec4 i_uv;      /* u0, v0, u1, v1 -- normalised  */
layout(location = 2) in vec4 i_color;

layout(location = 0)      out vec2 v_uv;
/* flat: one colour for the whole glyph, so there is nothing to interpolate. */
layout(location = 1) flat out vec4 v_color;

void main()
{
	/* Strip order is (0,0) (1,0) (0,1) (1,1): the low two bits of the index,
	 * so four vertices and no array. */
	vec2 k = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);

	v_uv    = mix(i_uv.xy, i_uv.zw, k);
	v_color = i_color;

	vec2 px = i_rect.xy + k * i_rect.zw;
	/* Pixel origin is top-left, matching Vulkan's NDC y-down. */
	gl_Position = vec4(px / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}
