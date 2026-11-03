/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 */

#version 450

/* The colour cube.vert lit and encoded for the target; nothing left to do. */
layout(location = 0) flat in vec3 v_color;

layout(location = 0) out vec4 o_color;

void main()
{
	o_color = vec4(v_color, 1.0);
}
