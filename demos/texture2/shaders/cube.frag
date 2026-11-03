/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 */

#version 450

/* cube.vert's block; this stage reads only srgb. */
layout(push_constant) uniform Push {
	mat4 mvp;
	vec4 light;
	uint srgb;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in float v_shade;

layout(location = 0) out vec4 o_color;

vec3 srgb_encode(vec3 c)
{
	return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
			   step(vec3(0.0031308), c));
}

void main()
{
	/* The texture is an sRGB image, so this is already linear -- and the
	 * light is applied there, the only space it is right in. */
	const vec3 lit = texture(tex, v_uv).rgb * v_shade;

	/* An _SRGB target encodes as it is written; anything else has to be
	 * encoded here or the picture comes out dark. */
	o_color = vec4(pc.srgb != 0u ? lit : srgb_encode(lit), 1.0);
}
