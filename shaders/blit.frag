/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The scene image back onto the window, one texel fetch per screen pixel.
 *
 * The behaviour is in the sampler: it is LINEAR (see create_scene_objects
 * in src/renderer/vulkan/scene_target.c), so this fetch is the bilinear filter
 * the render scale is resolved with.
 *
 * On an _SRGB surface the image is _SRGB too, so the hardware decodes each
 * texel to linear before filtering and re-encodes on write.
 *
 * The vertex stage is shaders/fullscreen.vert. The viewport places the
 * triangle, so the same unit-square uv covers whatever rectangle the letterbox
 * left.
 */

layout(set = 0, binding = 0) uniform sampler2D u_scene;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	out_color = vec4(texture(u_scene, v_uv).rgb, 1.0);
}
