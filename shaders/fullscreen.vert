/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The covering triangle, for every pass that fills whatever it is pointed at:
 * the fetch that scales the scene onto the window (shaders/blit.frag) and the
 * morphological filter between them (shaders/mlaa.frag).
 *
 * A caller's scene shader carries its own copy of these four lines rather than
 * sharing this one -- cheaper than a seam for a triangle.
 *
 * No vertex buffer and no push constant: uv (0,0), (2,0), (0,2) put the
 * clip-space corners at (-1,-1), (3,-1), (-1,3), one triangle covering the
 * viewport with the unit square inside it. Where it lands is the viewport's
 * business, which lets one pipeline fill an offscreen image and then a
 * letterboxed rectangle inside the window.
 *
 * uv runs y-down, like Vulkan's clip space and like the image's own rows, so
 * the blit needs no flip.
 */

layout(location = 0) out vec2 v_uv;

void main()
{
	v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
