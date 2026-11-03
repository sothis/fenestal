/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The covering triangle the test pattern is drawn with: uv (0,0), (2,0), (0,2)
 * from gl_VertexIndex, y-down, placed by the viewport.
 *
 * Duplicated from the library's own shaders/fullscreen.vert rather than shared
 * with it: sharing would mean the library publishing a shader, and promising
 * its interface across versions in a language whose linkage is by location
 * number.
 */

layout(location = 0) out vec2 v_uv;

void main()
{
	v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
