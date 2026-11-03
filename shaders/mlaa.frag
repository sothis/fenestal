/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * Morphological antialiasing over the rendered scene: find the stair-steps an
 * edge left in the image, work out how much of each pixel the edge really
 * covered, and blend accordingly. It reads the finished picture, so it works
 * on what a fragment shader drew, which multisampling cannot touch.
 *
 * One pass: from each pixel on an edge it walks along the edge both ways until
 * the edge stops, and takes the coverage from where the pixel sits in that
 * span. DESIGN *Antialiasing* has what that gives up against three-pass MLAA.
 *
 * Edges are found on luminance. The image is sampled through an _SRGB view
 * where the surface has one, so these samples are linear light; the weights
 * below are Rec. 709's.
 *
 * Every fetch is textureLod at level 0: most sit after an early return or in
 * a data-dependent loop, where implicit derivatives are undefined.
 */

layout(set = 0, binding = 0) uniform sampler2D u_scene;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

const float EDGE_THRESHOLD = 0.045;   /* below this a step is not an edge   */
const int   SEARCH_STEPS   = 12;      /* how far along an edge to walk      */
const float MIN_SPAN       = 4.0;     /* shorter is not a staircase (below)  */

float luma(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main()
{
	vec2 texel = 1.0 / vec2(textureSize(u_scene, 0));

	vec3 rgb_c = textureLod(u_scene, v_uv, 0.0).rgb;
	vec3 rgb_n = textureLod(u_scene, v_uv + vec2(0.0, -texel.y), 0.0).rgb;
	vec3 rgb_s = textureLod(u_scene, v_uv + vec2(0.0,  texel.y), 0.0).rgb;
	vec3 rgb_w = textureLod(u_scene, v_uv + vec2(-texel.x, 0.0), 0.0).rgb;
	vec3 rgb_e = textureLod(u_scene, v_uv + vec2( texel.x, 0.0), 0.0).rgb;
	float l_c = luma(rgb_c);
	float l_n = luma(rgb_n);
	float l_s = luma(rgb_s);
	float l_w = luma(rgb_w);
	float l_e = luma(rgb_e);

	float lo = min(l_c, min(min(l_n, l_s), min(l_w, l_e)));
	float hi = max(l_c, max(max(l_n, l_s), max(l_w, l_e)));
	if (hi - lo < EDGE_THRESHOLD) {
		out_color = vec4(rgb_c, 1.0);   /* nothing here to soften */
		return;
	}

	/* Which way the edge runs. A horizontal edge is a step in the vertical
	 * direction, so the second difference across y is the larger one. */
	bool horizontal = abs(l_n + l_s - 2.0 * l_c) >= abs(l_w + l_e - 2.0 * l_c);

	/* The neighbour across the edge -- the darker or lighter side we will be
	 * blending towards -- and the offset that reaches it. */
	float l_a = horizontal ? l_n : l_w;
	float l_b = horizontal ? l_s : l_e;
	bool  towards_b = abs(l_b - l_c) > abs(l_a - l_c);
	vec2  across = horizontal ? vec2(0.0, texel.y) : vec2(texel.x, 0.0);
	if (!towards_b)
		across = -across;

	/* The step across the edge, signed. Both halves of the search below test a
	 * neighbouring step against this one -- same size, same direction. */
	float delta     = (towards_b ? l_b : l_a) - l_c;
	float step_size = abs(delta);
	float step_dir  = sign(delta);
	if (step_size < EDGE_THRESHOLD) {
		out_color = vec4(rgb_c, 1.0);
		return;
	}

	/* Walk along the edge both ways while the same step is still there. */
	vec2 along = horizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
	float dist_neg = float(SEARCH_STEPS), dist_pos = float(SEARCH_STEPS);
	bool  end_neg = false, end_pos = false;

	for (int i = 1; i <= SEARCH_STEPS; i++) {
		if (!end_neg) {
			vec2  uv = v_uv - along * float(i);
			float d  = luma(textureLod(u_scene, uv + across, 0.0).rgb) -
					   luma(textureLod(u_scene, uv, 0.0).rgb);
			if (abs(abs(d) - step_size) > step_size * 0.5 ||
				sign(d) != step_dir) {
				dist_neg = float(i) - 1.0;
				end_neg = true;
			}
		}
		if (!end_pos) {
			vec2  uv = v_uv + along * float(i);
			float d  = luma(textureLod(u_scene, uv + across, 0.0).rgb) -
					   luma(textureLod(u_scene, uv, 0.0).rgb);
			if (abs(abs(d) - step_size) > step_size * 0.5 ||
				sign(d) != step_dir) {
				dist_pos = float(i) - 1.0;
				end_pos = true;
			}
		}
		if (end_neg && end_pos)
			break;
	}

	/* Coverage from where this pixel sits in the span: nothing to correct in
	 * the middle of a long step, half the pixel at its ends.
	 *
	 * An edge that never ended within the search is left alone: a long
	 * unbroken step is a real feature of the picture, like the pattern's grid
	 * lines. */
	float span = dist_neg + dist_pos + 1.0;
	if (!end_neg && !end_pos) {
		out_color = vec4(rgb_c, 1.0);
		return;
	}

	/* A span this short is not a staircase to reconstruct: the run's length is
	 * all the coverage is derived from, and blending a run of one to three
	 * turns a regular staircase into an irregular one, or erases a one-pixel
	 * feature. Four is about where a staircase stops reading as one -- some
	 * fourteen degrees off the axis. Measured on the test pattern: 2.5% of the
	 * pixels changed without this and 1.7% with it, against FSAA's 1.2%.
	 * DESIGN *Antialiasing*. */
	if (span < MIN_SPAN) {
		out_color = vec4(rgb_c, 1.0);
		return;
	}

	float nearest = min(dist_neg, dist_pos);
	float weight  = clamp(0.5 - (nearest + 0.5) / span, 0.0, 0.5);

	/* The neighbour `across` reaches, which the first fetches already hold. */
	vec3 rgb_x = horizontal ? (towards_b ? rgb_s : rgb_n)
							: (towards_b ? rgb_e : rgb_w);
	out_color = vec4(mix(rgb_c, rgb_x, weight), 1.0);
}
