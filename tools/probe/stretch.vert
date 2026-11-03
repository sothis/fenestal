/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/* One triangle covering the target, uv 0..1 over it (stretch.c). */
layout(location = 0) out vec2 v_uv;
void main()
{
	v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
