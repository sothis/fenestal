/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 */

#version 450

/* One block, shared with cube.frag and with CubePush in texture2.h, whose
 * asserts pin the three together. */
layout(push_constant) uniform Push {
	mat4 mvp;
	vec4 light;     /* xyz: towards the light, in the cube's own space */
	uint srgb;      /* the target is _SRGB and encodes as it is written */
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out float v_shade;

/* The cube is generated here from gl_VertexIndex, so the scene has no vertex
 * buffer: 36 vertices, six per face. Each face is its normal and two edge
 * directions with u x v = n, which makes the corners below run
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

void main()
{
	const int face = gl_VertexIndex / 6;
	const vec2 c = CORNER[TRI[gl_VertexIndex % 6]];
	const vec3 n = NORMAL[face];
	const vec3 pos = 0.5 * (n + c.x * EDGE_U[face] + c.y * EDGE_V[face]);
	gl_Position = pc.mvp * vec4(pos, 1.0);

	/* The whole texture on every face, upright: v = 0 at the face's top. */
	v_uv = vec2(0.5 + 0.5 * c.x, 0.5 - 0.5 * c.y);

	/* A face is flat, so its light is one figure for all of it: ambient
	 * and diffuse, in linear, where cube.frag multiplies it in. */
	v_shade = 0.18 + 0.82 * max(dot(n, pc.light.xyz), 0.0);
}
