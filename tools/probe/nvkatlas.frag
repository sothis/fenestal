/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/* The atlas sampled as the library's text does -- texture() through a
 * NEAREST sampler -- at each texel's centre, into a target its own size
 * (nvkatlas.c). The coverage goes to every channel. */
layout(set = 0, binding = 0) uniform sampler2D u_atlas;
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(texture(u_atlas, v_uv).rrr, 1.0); }
