/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * Antialiased thick stroke, evaluated analytically: the fragment's exact
 * distance to the shape drives a smooth coverage ramp, so edges stay clean at
 * any angle and any surface scale. Every titlebar button icon is built from
 * these (see src/core/draw.c).
 *
 * Coverage feeds alpha and the pipeline blends, so *overlapping* draws are a
 * hazard: two strokes crossing at partial coverage blend over each other, and
 * 1-(1-a)^2 is denser than a. Shapes that meet are therefore one primitive --
 * a segment, two meeting at a bend, or a closed rectangle.
 *
 * The stroke arrives from the vertex stage as flat varyings, one instance per
 * stroke.
 */

layout(location = 0)      in vec2 v_px;
layout(location = 1) flat in vec4 v_p0p1;   /* p0.xy, p1.xy           */
layout(location = 2) flat in vec4 v_p2hf;   /* p2.xy, half_w, feather */
layout(location = 3) flat in vec4 v_color;
layout(location = 4) flat in vec4 v_color2; /* the gradient's second end */
layout(location = 5) flat in float v_kind;  /* ShapeKind               */

layout(location = 0) out vec4 out_color;

/* Distance from p to the capsule spine a..b. */
float segment_distance(vec2 p, vec2 a, vec2 b)
{
	vec2  pa = p - a;
	vec2  ba = b - a;
	float len2 = dot(ba, ba);
	/* len2 == 0 is a degenerate segment, i.e. a round dot at a. */
	float h = len2 > 0.0 ? clamp(dot(pa, ba) / len2, 0.0, 1.0) : 0.0;
	return length(pa - ba * h);
}

/* Distance from p to the *border* of the axis-aligned rectangle a..b: the
 * signed distance to the filled box, made unsigned so that inside and outside
 * fall away from the border alike. */
float box_border_distance(vec2 p, vec2 a, vec2 b)
{
	vec2 half_e = 0.5 * abs(b - a);
	vec2 q = abs(p - 0.5 * (a + b)) - half_e;
	return abs(min(max(q.x, q.y), 0.0) + length(max(q, 0.0)));
}

/* The signed distance to a box a..b whose corners are rounded by `r`: the
 * length of the overflow past the box shrunk by r, less r. Negative inside,
 * so the coverage ramp straddles the border; r == 0 is the plain box. */
float box_round_distance(vec2 p, vec2 a, vec2 b, float r)
{
	vec2 half_e = 0.5 * abs(b - a) - r;
	vec2 q = abs(p - 0.5 * (a + b)) - half_e;
	return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main()
{
	float half_w  = v_p2hf.z;
	float feather = v_p2hf.w;
	float coverage;

	if (v_kind > 2.5 && v_kind < 3.5) {
		/* A gradient: t is the position along the box's x axis turned by
		 * half_w degrees, and the two colours are the ends. No coverage ramp
		 * -- the box is the whole primitive -- so the edge is the scissor's. */
		float a = radians(half_w);
		vec2  dir = vec2(cos(a), sin(a));
		vec2  lo = v_p0p1.xy, hi = v_p0p1.zw;
		/* The rectangle's extent along the axis, and the corner the axis
		 * enters by: the low corner on each axis the direction runs
		 * forward on and the high one where it runs back. The extent is
		 * not the diagonal's projection -- that is right only where the
		 * two components share a sign, and is zero on a square at 45
		 * degrees and its reverse, which would paint it flat. A box with
		 * no extent along the axis has no ramp to run, and its answer is
		 * the first colour. */
		const vec2  size = abs(hi - lo);
		const float ext  = size.x * abs(dir.x) + size.y * abs(dir.y);
		vec2 from = vec2(dir.x >= 0.0 ? lo.x : hi.x,
						 dir.y >= 0.0 ? lo.y : hi.y);
		float t = ext > 1e-6 ? dot(v_px - from, dir) / ext : 0.0;
		out_color = mix(v_color, v_color2, clamp(t, 0.0, 1.0));
		return;
	}

	if (v_kind > 0.5) {
		/* A filled box: the signed distance to the box, rounded where the kind
		 * says so -- 1 inside, falling to 0 across the feather at the border --
		 * so the interior is opaque whatever half_w says. */
		float sd = v_kind > 1.5
				   ? box_round_distance(v_px, v_p0p1.xy, v_p0p1.zw, half_w)
				   : box_round_distance(v_px, v_p0p1.xy, v_p0p1.zw, 0.0);
		coverage = 1.0 - smoothstep(-feather, feather, sd);
	} else {
		/* One distance for the whole shape, so a join is covered once and is
		 * exactly as opaque as the arms leading into it.
		 *
		 * Which shape is told by p2, with no flag beside it: p2 == p1 collapses
		 * the second arm to a point inside the first, a plain line; p2 == p0 with
		 * p0 != p1 is the one thing an elbow can never mean, so it is read as p0
		 * and p1 being opposite corners of a rectangle. */
		float d = (v_p2hf.xy == v_p0p1.xy && v_p0p1.xy != v_p0p1.zw)
					? box_border_distance(v_px, v_p0p1.xy, v_p0p1.zw)
					: min(segment_distance(v_px, v_p0p1.xy, v_p0p1.zw),
						  segment_distance(v_px, v_p0p1.zw, v_p2hf.xy));
		coverage = 1.0 - smoothstep(half_w - feather, half_w + feather, d);
	}
	if (coverage <= 0.0)
		discard;

	out_color = vec4(v_color.rgb, v_color.a * coverage);
}
