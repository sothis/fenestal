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
 * much; sampling at `+ jitter` puts it back. What that buys is a fresh set of
 * subpixel samples per frame, which the mix gathers into one.
 *
 * The history is the last frame this pass wrote and is mixed in at
 * pc.jitter.z. Where the scene moved, pc.texel.zw says how far and the
 * history is read where the picture used to be, so moving content is
 * gathered rather than dropped; it is then *clamped* to the range its 3x3
 * neighbourhood has in this frame before it is mixed. That clamp is the
 * whole of the motion handling beyond the vector -- an edge the box cannot
 * hold is passed over rather than smeared. Only the history's lookup is
 * reprojected, not the picture, which is why this is temporal antialiasing
 * that happens to sit at a smaller grid than the window and not a
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
layout(set = 0, binding = 2) uniform sampler2D u_depth;

layout(push_constant) uniform Push {
	vec4 jitter;  /* xy: this frame's offset in scene pixels; z: history weight */
	vec4 texel;   /* xy: 1 / the scene's extent, zw: the scene's motion in px */
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
	/* Where this frame's scene put the picture's own pixel: the caller
	 * moved the picture by the jitter, so the sample is taken that far
	 * forward along it -- half a texel out at the edge, which CLAMP_TO_EDGE
	 * holds. */
	const vec2 uv  = v_uv + pc.jitter.xy * pc.texel.xy;
	/* Where the picture now at v_uv was in the frame before: the scene's
	 * motion undone. Zero is the default, and then this is v_uv and the
	 * history is read under the pixel it is blended into. */
	const vec2 huv = v_uv - pc.texel.zw * pc.texel.xy;
	const vec2 t   = pc.texel.xy;

	const vec3 now = texture(u_scene, uv).rgb;
	const vec3 l   = texture(u_scene, uv - vec2(t.x, 0.0)).rgb;
	const vec3 r   = texture(u_scene, uv + vec2(t.x, 0.0)).rgb;
	const vec3 d   = texture(u_scene, uv - vec2(0.0, t.y)).rgb;
	const vec3 u   = texture(u_scene, uv + vec2(0.0, t.y)).rgb;
	/* The corners too: the box is the 3x3 neighbourhood this pass documents,
	 * and a diagonal edge is exactly what the four neighbours alone cannot
	 * bound -- its history would be dropped where the accumulation is meant
	 * to smooth it. */
	const vec3 dl  = texture(u_scene, uv + vec2(-t.x, -t.y)).rgb;
	const vec3 dr  = texture(u_scene, uv + vec2(t.x, -t.y)).rgb;
	const vec3 ul  = texture(u_scene, uv + vec2(-t.x, t.y)).rgb;
	const vec3 ur  = texture(u_scene, uv + vec2(t.x, t.y)).rgb;

	/* What this frame can hold where the pixel is, which is what the history
	 * is clamped into -- not where the history was read: the box is a
	 * statement about this frame, and the reprojected sample has to land
	 * inside it or the picture has moved more than the clamp allows. */
	const vec3 lo = min(now, min(min(min(l, r), min(d, u)),
								 min(min(dl, dr), min(ul, ur))));
	const vec3 hi = max(now, max(max(max(l, r), max(d, u)),
								 max(max(dl, dr), max(ul, ur))));

	/* The depth at this pixel, which the resolve carries out in its alpha:
	 * the history copy takes the whole image, and alpha is the one channel
	 * an _SRGB target does not transform, so the depth it reads back next
	 * frame is the depth it wrote. The fetch reads .rgb and ignores it. */
	const float dep = textureLod(u_depth, uv, 0.0).r;
	const vec4 prev = texture(u_history, huv);
	/* The depth's reciprocal, a relative step in which is the test: on a
	 * reverse-Z buffer it is proportional to eye distance, and on the
	 * standard [0,1] depth the tree's own scenes use it rises with it and
	 * saturates far away -- a step there is coarser than a step in metres.
	 * A history sample further than the threshold behind this frame is the
	 * picture having moved across a discontinuity, and is dropped rather
	 * than smeared; at nought the compare is off, which is every renderer
	 * that has no depth to compare. */
	const float ec = 1.0 / max(dep, 1e-5);
	const float eh = 1.0 / max(prev.w, 1e-5);
	const bool moved_on =
		pc.jitter.w > 0.0 && abs(eh - ec) > pc.jitter.w * ec;

	/* At weight nought the history holds nothing worth having -- a slot's
	 * image before its first copy, or the frame after a rest -- and is not
	 * read at all, rather than mixed in where a value from unwritten memory
	 * would decide the result. */
	const vec3 hist = pc.jitter.z > 0.0 && !moved_on
		? clamp(prev.rgb, lo, hi)
		: now;

	out_color = vec4(mix(now, hist, pc.jitter.z), dep);
}
