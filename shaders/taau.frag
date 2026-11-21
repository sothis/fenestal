/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * The scene onto the window, temporally: this frame's image resolved against
 * the frames before it, each sampled where the offset it was rasterised at put
 * the pixel (DESIGN *Antialiasing*).
 *
 * The caller offsets its projection by FnlFrame::jitter_x every frame (see
 * fenestal.h), so the image is the scene shifted off the pixel grid by that
 * much; sampling at `- jitter` puts it back. What that buys is a fresh set of
 * subpixel samples per frame, which the mix gathers into one.
 *
 * The history is the last frame this pass wrote and is mixed in at
 * pc.jitter.z. There are no motion vectors here, so it is *clamped* to the
 * range its 3x3 neighbourhood has in this frame before it is mixed: moving
 * content contributes only what it left inside that box, and an edge the box
 * cannot hold is dropped rather than smeared. That clamp is the whole of the
 * motion handling, and it is why this is temporal antialiasing and not a
 * reprojecting upscale.
 *
 * Both images are the surface's format and _SRGB with it, so the hardware
 * decodes before filtering and re-encodes on write: the mix is in linear
 * light, as every resolve here is (DESIGN *Colour*).
 *
 * The vertex stage is shaders/fullscreen.vert, and the viewport places the
 * triangle over the rectangle the scene lands on.
 */

layout(set = 0, binding = 0) uniform sampler2D u_scene;
layout(set = 0, binding = 1) uniform sampler2D u_history;

layout(push_constant) uniform Push {
	vec4 jitter;  /* xy: this frame's offset in scene pixels; z: history weight */
	vec4 texel;   /* xy: 1 / the scene's extent                                  */
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	/* Where this frame's scene put this pixel: the offset undone, and half a
	 * texel out at the edge, which CLAMP_TO_EDGE holds. */
	const vec2 uv = v_uv - pc.jitter.xy * pc.texel.xy;
	const vec2 t  = pc.texel.xy;

	const vec3 now = texture(u_scene, uv).rgb;
	const vec3 l   = texture(u_scene, uv - vec2(t.x, 0.0)).rgb;
	const vec3 r   = texture(u_scene, uv + vec2(t.x, 0.0)).rgb;
	const vec3 d   = texture(u_scene, uv - vec2(0.0, t.y)).rgb;
	const vec3 u   = texture(u_scene, uv + vec2(0.0, t.y)).rgb;

	const vec3 lo = min(now, min(min(l, r), min(d, u)));
	const vec3 hi = max(now, max(max(l, r), max(d, u)));

	/* No reprojection, so the pixel to blend is the same pixel: whatever of
	 * the previous frame lies outside what this one can hold is the picture
	 * having moved, and is passed over. At weight nought the history holds
	 * nothing worth having -- a slot's image before its first copy, or the
	 * frame after a rest -- and is not read at all, rather than mixed in
	 * where a value from unwritten memory would decide the result. */
	const vec3 hist = pc.jitter.z > 0.0
		? clamp(texture(u_history, v_uv).rgb, lo, hi)
		: now;

	out_color = vec4(mix(now, hist, pc.jitter.z), 1.0);
}
