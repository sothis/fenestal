/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

#version 450
/*
 * Glyph coverage -> screen, from the atlas tools/mkfont bakes.
 *
 * Compiled twice. With SUBPIXEL defined it does LCD subpixel antialiasing and
 * needs dual-source blending; without, it takes the middle subpixel as plain
 * grayscale coverage. text.c picks by whether the device offers dualSrcBlend.
 *
 * A texel is coverage: the fraction of that sample's area the outline covers,
 * baked for exactly the pixel grid the quad lands on and read through a
 * NEAREST sampler -- a lookup, not a filter. It is NOT a colour, so no
 * transfer function applies to it.
 *
 * The atlas is three texels wide per pixel, one per LCD emitter (see
 * src/renderer/vulkan/text.c), and v_uv at a pixel centre is the middle one.
 * Per-channel coverage cannot go through a single alpha, so the subpixel
 * variant hands the blender a weight per channel in a second colour output.
 *
 * ---- the blend ------------------------------------------------------------
 *
 * Ground truth for antialiased text is a box filter over radiance,
 *
 *     L = c * L_ink + (1 - c) * L_bg                                     (1)
 *
 * a linear-light interpolation weighted by the raw coverage. The blender must
 * be handed a weight that makes it compute (1) in the space it blends in.
 *
 *   _SRGB surface
 *       The hardware decodes dst, blends in linear light and encodes on write,
 *       so (1) is what the blender does and the weight is the coverage
 *       untouched.
 *
 *   UNORM surface (the fallback)
 *       The blender works on stored, sRGB-encoded values with no conversion
 *       either side -- the wrong space, and fixed-function cannot decode dst
 *       for us. Over a dark background the compensation is exact though:
 *       wanting displayed luminance a * L_ink from a blend that yields stored
 *       a' * S_ink, approximating the transfer as a pure 2.2 power,
 *
 *           (a' * S_ink)^2.2 = a * (S_ink)^2.2   ->   a' = a^(1/2.2)
 *
 *       so this path wants coverage *encoded*. The panels it draws on are near
 *       enough black that the residual is invisible.
 *
 * ---- perceptual shaping ---------------------------------------------------
 *
 * (1) is physically right and still reads soft at a ten-pixel cap height,
 * because a correct ramp spreads an edge over a pixel the eye then integrates
 * with its neighbours. Both constants below act on coverage, before any
 * transfer function, because that is where "how much ink" means anything.
 * Neutral values (0.0 / 1.0) give back exactly (1).
 */

/* Ramp narrowing, [0,1]. Mixes toward a Hermite S-curve: monotone, both
 * endpoints fixed, so it sharpens without shifting stroke weight or clipping
 * the ramp into aliasing the way a gain about the midpoint does. */
const float CONTRAST = 0.80;

/* Stroke weight. >1 thins, <1 fattens. Light-on-dark strokes bloom and read
 * heavier than their coverage, so this trims them back. */
const float GAMMA = 1.25;

/* Emitter order across a pixel: a property of the panel, which the program
 * cannot measure. Wrong, the fringes still balance but each stem shifts two
 * thirds of a pixel and the text reads blurred rather than sharper than
 * grayscale -- so this is the first thing to flip if subpixel looks worse. */
const bool BGR = false;

layout(push_constant) uniform Push {
	vec2  screen;         /* framebuffer size, pixels -- the vertex stage's */
	uint  srgb;           /* nonzero when the surface blends in linear light */
	float texel_u;        /* one atlas texel across, normalised */
} pc;

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0)      in vec2 v_uv;
layout(location = 1) flat in vec4 v_color;

#ifdef SUBPIXEL
/* index 0 is the ink; index 1 is the per-channel weight the blender applies to
 * it. Both are location 0 -- that is what dual-source blending means. */
layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_weight;
#else
layout(location = 0) out vec4 out_color;
#endif

/* linear -> sRGB. The encode, needed only on the UNORM fallback. */
float to_srgb(float c)
{
	return c <= 0.0031308 ? c * 12.92
						  : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/* coverage -> the weight the blender wants, for one channel */
float shape(float cov)
{
	float a = mix(cov, smoothstep(0.0, 1.0, cov), CONTRAST);
	a = pow(a, GAMMA);
	return pc.srgb == 0u ? to_srgb(a) : a;
}

void main()
{
#ifdef SUBPIXEL
	/* One texel either side of the pixel centre: the red and blue emitters.
	 * The step is pushed per draw because the atlas, and its width with it,
	 * is replaced when the surface scale changes; zero collapses the three
	 * samples to grayscale. */
	float du = pc.texel_u;
	float lo = texture(atlas, vec2(v_uv.x - du, v_uv.y)).r;   /* leftmost   */
	float mid = texture(atlas, v_uv).r;
	float hi = texture(atlas, vec2(v_uv.x + du, v_uv.y)).r;   /* rightmost  */
	vec3 cov = BGR ? vec3(hi, mid, lo) : vec3(lo, mid, hi);
	if (cov == vec3(0.0))
		discard;

	vec3 w = vec3(shape(cov.r), shape(cov.g), shape(cov.b)) * v_color.a;

	out_color  = vec4(v_color.rgb, 1.0);
	/* The alpha the blender writes to the surface: the pixel's overall
	 * coverage (DESIGN *Composite alpha*). */
	out_weight = vec4(w, max(w.r, max(w.g, w.b)));
#else
	/* The middle subpixel is the FIR5 filter's weighted average of the three,
	 * which is the grayscale coverage. */
	float cov = texture(atlas, v_uv).r;
	if (cov <= 0.0)
		discard;
	out_color = vec4(v_color.rgb, v_color.a * shape(cov));
#endif
}
