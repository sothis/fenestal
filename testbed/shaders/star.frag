/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The star's wedges: one flat value, already converted in the vertex stage.
 * All of the interesting behaviour is there and in the rasteriser -- the point
 * of drawing these as triangles is that their edges are covered *partially*,
 * which is the thing multisampling can average and a fragment shader cannot
 * fake.
 */

layout(location = 0) flat in  float v_shade;
layout(location = 0)      out vec4  out_color;

void main()
{
	out_color = vec4(vec3(v_shade), 1.0);
}
