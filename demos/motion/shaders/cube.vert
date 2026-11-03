/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 */

#version 450

/* One block, shared with CubePush in motion.h, whose asserts pin the two
 * together. */
layout(push_constant) uniform Push {
	mat4 mvp;
	vec4 light;     /* xyz: towards the light, in the cube's own space */
	uint srgb;      /* the target is _SRGB and encodes as it is written */
} pc;

layout(location = 0) flat out vec3 v_color;

/* The cube is generated here from gl_VertexIndex, so the scene has no vertex
 * buffer at all: 36 vertices, six per face. Each face is its normal and two
 * edge directions with u x v = n, which makes the corners below run
 * counter-clockwise seen from outside on every face. */
const vec3 NORMAL[6] = vec3[](
	vec3( 1, 0, 0), vec3(-1, 0, 0), vec3(0,  1, 0),
	vec3(0, -1, 0), vec3(0, 0,  1), vec3(0, 0, -1));
const vec3 EDGE_U[6] = vec3[](
	vec3(0, 0, -1), vec3(0, 0, 1), vec3(1, 0, 0),
	vec3(1, 0,  0), vec3(1, 0, 0), vec3(-1, 0, 0));
const vec3 EDGE_V[6] = vec3[](
	vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, -1),
	vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1,  0));
const vec2 CORNER[4] = vec2[](
	vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, 1));
const int TRI[6] = int[](0, 1, 2, 0, 2, 3);

/* A colour a face, as sRGB: the numbers a colour picker gives. */
const vec3 COLOR[6] = vec3[](
	vec3(0.92, 0.33, 0.28), vec3(0.20, 0.72, 0.80), vec3(0.40, 0.80, 0.35),
	vec3(0.78, 0.36, 0.72), vec3(0.28, 0.47, 0.90), vec3(0.95, 0.80, 0.30));

vec3 srgb_decode(vec3 c)
{
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
			   step(vec3(0.04045), c));
}

vec3 srgb_encode(vec3 c)
{
	return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
			   step(vec3(0.0031308), c));
}

void main()
{
	const int face = gl_VertexIndex / 6;
	const vec2 c = CORNER[TRI[gl_VertexIndex % 6]];
	const vec3 n = NORMAL[face];
	const vec3 pos = 0.5 * (n + c.x * EDGE_U[face] + c.y * EDGE_V[face]);
	gl_Position = pc.mvp * vec4(pos, 1.0);

	/* Lit here, once a vertex: a face is flat, so every vertex of it gets
	 * the same answer. In linear, the only space lighting is right in. */
	const float diffuse = max(dot(n, pc.light.xyz), 0.0);
	const vec3 lit = srgb_decode(COLOR[face]) * (0.18 + 0.82 * diffuse);

	/* An _SRGB target encodes as it is written; anything else has to be
	 * encoded here or the picture comes out dark. */
	v_color = pc.srgb != 0u ? lit : srgb_encode(lit);
}
