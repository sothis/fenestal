/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The scene image back onto the window: a bicubic resolve per screen pixel,
 * where this was one bilinear fetch.
 *
 * The sampler is LINEAR (see create_scene_objects in
 * src/renderer/vulkan/scene_target.c) and that is what makes a 4x4 cubic
 * affordable: every tap is one bilinear fetch, so the sixteen texels cost
 * sixteen fetches and not sixty-four.
 *
 * The cubic is Catmull-Rom, the interpolating one. At 1:1 -- an antialias
 * mode at the scene's own size, where this fetch copies the image to the
 * window -- the fraction is nought, the outer two weights vanish and the
 * result is the texel itself: the identity, so a caller that pins no render
 * scale pays what it paid for a bilinear fetch. Its negative lobe is what a
 * bilinear stretch rounds off, which is the sharpening; a second unsharp
 * pass would double it, and would want gating off at 1:1 that the fetch has
 * no way to be told.
 *
 * The taps are in a loop, where an implicit derivative is undefined, so each
 * is textureLod at level 0 -- which is also the only level, the sampler
 * having no mipmaps. On an _SRGB image the hardware decodes each texel to
 * linear before filtering, so the weighted sum is in linear light, as every
 * resolve here is (DESIGN *Colour*).
 *
 * The vertex stage is shaders/fullscreen.vert. The viewport places the
 * triangle, so the same unit-square uv covers whatever rectangle the
 * letterbox left.
 */

layout(set = 0, binding = 0) uniform sampler2D u_scene;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	const vec2 texel = 1.0 / vec2(textureSize(u_scene, 0));

	/* Texel space, where texel i covers [i, i + 1): the fraction is where
	 * the sample sits between the two texels it straddles, and the four
	 * taps are the two either side of that pair. */
	const vec2 pos = v_uv / texel - 0.5;
	const vec2 f   = fract(pos);
	const vec2 b   = pos - f;

	/* Catmull-Rom, per axis: the interpolating cubic, its outer two
	 * weights negative, which is the lobe that sharpens. The four sum to
	 * one whatever the fraction. */
	const float x2 = f.x * f.x, x3 = x2 * f.x;
	const float y2 = f.y * f.y, y3 = y2 * f.y;
	const vec4 wx  = vec4(-0.5 * x3 + x2 - 0.5 * f.x,
						 1.5 * x3 - 2.5 * x2 + 1.0,
						-1.5 * x3 + 2.0 * x2 + 0.5 * f.x,
						 0.5 * x3 - 0.5 * x2);
	const vec4 wy  = vec4(-0.5 * y3 + y2 - 0.5 * f.y,
						 1.5 * y3 - 2.5 * y2 + 1.0,
						-1.5 * y3 + 2.0 * y2 + 0.5 * f.y,
						 0.5 * y3 - 0.5 * y2);

	vec3 c = vec3(0.0);
	for (int j = 0; j < 4; ++j) {
		const float kj = wy[j];
		for (int i = 0; i < 4; ++i) {
			/* Texel b + (i, j) - 1, whose centre is at +0.5. */
			const vec2 o = (b + vec2((float)i, (float)j) - 0.5) * texel;
			c += textureLod(u_scene, o, 0.0).rgb * (wx[i] * kj);
		}
	}

	out_color = vec4(c, 1.0);
}
