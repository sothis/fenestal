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
 * affordable: every tap is one bilinear fetch, and the middle two texels of
 * an axis share one, so the sixteen texels cost nine fetches and not
 * sixty-four.
 *
 * The cubic is Catmull-Rom, the interpolating one. At 1:1 -- an antialias
 * mode at the scene's own size, where this fetch copies the image to the
 * window -- the fraction is nought, the outer two weights vanish and the
 * result is the texel itself: the identity, and the shader takes the one tap
 * for it, the sixteen having measured +0.26 ms at 1440p for an answer it
 * already has. Its negative lobe is what a bilinear stretch rounds off, which
 * is the sharpening; a second unsharp pass would double it, and would want
 * gating off at 1:1 that the fetch has no way to be told.
 *
 * Each tap is textureLod at level 0 -- the only level, the sampler having no
 * mipmaps -- so no implicit derivative is taken of a fetch that branches. On
 * an _SRGB image the hardware decodes each texel to linear before filtering,
 * so the weighted sum is in linear light, as every resolve here is (DESIGN
 * *Colour*).
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

	/* A sample on a texel's centre is that texel. The tolerance is about
	 * what pos itself carries at 4K (a float's ulp near 2048 is 0.0005),
	 * and at it the dropped outer weights are under a thousandth, below
	 * what the target keeps. Whole quads agree, so the branch is
	 * coherent: at 1:1 every pixel takes it, in a stretch none does. */
	const vec2 near = round(pos);
	if (all(lessThan(abs(pos - near), vec2(1.0 / 1024.0)))) {
		out_color = vec4(textureLod(u_scene, (near + 0.5) * texel, 0.0).rgb,
						 1.0);
		return;
	}

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

	/* Nine fetches, not sixteen. The two middle weights of Catmull-Rom are
	 * never negative, so the two texels they weigh are one bilinear fetch
	 * placed between them -- at their weights' ratio, scaled by their sum --
	 * and the sampler's own filter does the blend. The outer two weights
	 * are negative and keep their own fetch. Per axis that is three taps
	 * (outer, merged, outer), and the tensor product of three by three is
	 * the same cubic; the only difference from sixteen exact fetches is the
	 * fraction the filter keeps, which is eight bits. */
	const vec2 w12 = vec2(wx.y + wx.z, wy.y + wy.z);
	const vec2 p0  = (b - 0.5) * texel;                     /* texel b - 1 */
	const vec2 p3  = (b + 2.5) * texel;                     /* texel b + 2 */
	const vec2 p1  = (b + 0.5 + vec2(wx.z, wy.z) / w12) * texel;

	vec3 c = wy.x * (wx.x * textureLod(u_scene, vec2(p0.x, p0.y), 0.0).rgb +
					 w12.x * textureLod(u_scene, vec2(p1.x, p0.y), 0.0).rgb +
					 wx.w * textureLod(u_scene, vec2(p3.x, p0.y), 0.0).rgb);
	c += w12.y * (wx.x * textureLod(u_scene, vec2(p0.x, p1.y), 0.0).rgb +
				  w12.x * textureLod(u_scene, vec2(p1.x, p1.y), 0.0).rgb +
				  wx.w * textureLod(u_scene, vec2(p3.x, p1.y), 0.0).rgb);
	c += wy.w * (wx.x * textureLod(u_scene, vec2(p0.x, p3.y), 0.0).rgb +
				 w12.x * textureLod(u_scene, vec2(p1.x, p3.y), 0.0).rgb +
				 wx.w * textureLod(u_scene, vec2(p3.x, p3.y), 0.0).rgb);

	out_color = vec4(c, 1.0);
}
