/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The window's linear image into a PQ swapchain: sRGB's primaries to
 * BT.2020's, then SMPTE ST 2084, with SDR white at 203 nits -- BT.2408's
 * graphics white -- into a 1000-nit master. The one place PQ exists in the
 * renderer: every pass before this one blends in linear light, which PQ code
 * values are not, so the overlay is drawn into the linear image too and
 * encoded here with the scene (DESIGN *Colour*).
 *
 * The vertex stage is shaders/fullscreen.vert, as the fetch's: the viewport
 * places the triangle, so the same unit-square uv covers the window.
 */

layout(set = 0, binding = 0) uniform sampler2D u_image;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

/* sRGB's primaries into BT.2020's, linear light both sides and D65 both
 * sides: the matrix ITU-R BT.2087 gives. Column-major, as GLSL reads it. */
const mat3 TO_2020 = mat3(
	0.6274, 0.0691, 0.0164,
	0.3293, 0.9195, 0.0880,
	0.0433, 0.0114, 0.8956);

/* The nits a code value means: SDR white, and PQ's own ceiling. */
const float SDR_NITS     = 203.0;
const float PQ_PEAK_NITS = 10000.0;

/* ST 2084, on light in nits over 10000. */
vec3 pq(vec3 c)
{
	const float m1 = 2610.0 / 16384.0;
	const float m2 = 2523.0 / 4096.0 * 128.0;
	const float c1 = 3424.0 / 4096.0;
	const float c2 = 2413.0 / 4096.0 * 32.0;
	const float c3 = 2392.0 / 4096.0 * 32.0;
	const vec3 l = pow(max(c, 0.0), vec3(m1));
	return pow((c1 + c2 * l) / (1.0 + c3 * l), vec3(m2));
}

void main()
{
	const vec4 texel = texture(u_image, v_uv);
	/* The alpha passes through: it is the grab ring's (zero outside the
	 * window) as much as the overlay's, and the swapchain's composite alpha
	 * is what shows it. */
	out_color = vec4(pq(TO_2020 * texel.rgb * (SDR_NITS / PQ_PEAK_NITS)),
					 texel.a);
}
