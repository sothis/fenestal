/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * A caller's picture, one texel fetch a pixel, blended as it stands.
 *
 * The format is RGBA8, not premultiplied (FNL_IMAGE_RGBA8), so the straight
 * alpha is what the pipeline blends "over" with (make_overlay_pipeline()).
 * The sampler filters and clamps; on an _SRGB surface the image's format is
 * _SRGB too, so the hardware decodes each texel to linear before the blend
 * and the framebuffer re-encodes on write, as shaders/blit.frag does.
 *
 * The vertex stage is shaders/image.vert, and its uv is the quad's own
 * corner, so this is the image's row and column directly.
 */

layout(set = 0, binding = 0) uniform sampler2D u_image;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	out_color = texture(u_image, v_uv);
}
