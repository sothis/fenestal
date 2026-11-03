/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * Solid fill. These rectangles replace vkCmdClearAttachments, which writes
 * rather than blends, so the pipeline disables blending and this hands over
 * the colour untouched (see OVERLAY_BLEND_NONE in
 * src/renderer/vulkan/vk_internal.h).
 *
 * The colour is already in the surface's blend space -- app_palette_init()
 * converted it once, when the format was resolved.
 */

layout(location = 0) flat in vec4 v_color;
layout(location = 0) out vec4 out_color;

void main()
{
	out_color = v_color;
}
