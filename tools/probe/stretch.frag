/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/* shaders/blit.frag's fetch, for stretch.c. */
layout(set = 0, binding = 0) uniform sampler2D u_scene;
layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() { out_color = vec4(texture(u_scene, v_uv).rgb, 1.0); }
