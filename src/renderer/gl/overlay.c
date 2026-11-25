/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The overlay under OpenGL: rectangles, strokes and text, each one instanced
 * draw per flush, from the frame slot's arenas; the programs that draw them
 * and the scene's fetch; and the glyph atlas. What is drawn, and in which
 * order, is the core's (src/core/overlay.c, draw.c); this file is how.
 *
 * Every program is linked from spirv-cross's translation of the library's
 * shaders (DESIGN *One shader source*), so the GLSL here is the Vulkan GLSL,
 * and their `Push` blocks are fed through one uniform buffer at point 0.
 * Two translations of each: GLSL 4.50 for the 4.5 profile, and 4.10 for
 * the 4.1 one, whose vertex stages map Vulkan's depth range to GL's
 * themselves (DESIGN *OpenGL 4.1*).
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"

#include "gl/rect.vert.h"
#include "gl/rect.frag.h"
#include "gl/shape.vert.h"
#include "gl/shape.frag.h"
#include "gl/image.vert.h"
#include "gl/image.frag.h"
#include "gl/text.vert.h"
#include "gl/text_lcd.frag.h"
#include "gl/fullscreen.vert.h"
#include "gl/blit.frag.h"
#include "gl/mlaa.frag.h"
#include "gl/taau.frag.h"
#include "gl41/rect.vert.h"
#include "gl41/rect.frag.h"
#include "gl41/shape.vert.h"
#include "gl41/shape.frag.h"
#include "gl41/image.vert.h"
#include "gl41/image.frag.h"
#include "gl41/text.vert.h"
#include "gl41/text_lcd.frag.h"
#include "gl41/fullscreen.vert.h"
#include "gl41/blit.frag.h"
#include "gl41/mlaa.frag.h"
#include "gl41/taau.frag.h"
#include "font_data.h"
#include "font_codec.h"

/* The two `Push` blocks, as their std140 uniform blocks lay them out: the
 * same bytes as the Vulkan renderer's OverlayPush and TextPush, which is what
 * DESIGN *One shader source* rests on. Asserted like those. */
typedef struct {
	float screen[2];
} GlOverlayPush;

typedef struct {
	float screen[2];
	uint32_t srgb;
	float texel_u;
} GlTextPush;

/* The image pipeline's, as std140 lays it out: the same bytes as the Vulkan
 * renderer's ImagePush, which is what DESIGN *One shader source* rests on. */
typedef struct {
	float rect[4];
	float screen[2];
	float pad[2];
} GlImagePush;

_Static_assert(sizeof(GlOverlayPush) == 8,
			   "GlOverlayPush must match shaders/*.vert");
_Static_assert(sizeof(GlTextPush) == 16,
			   "GlTextPush must match shaders/text.*");
_Static_assert(offsetof(GlTextPush, srgb) == 8, "GlTextPush layout drifted");
_Static_assert(offsetof(GlTextPush, texel_u) == 12,
			   "GlTextPush layout drifted");
_Static_assert(sizeof(GlImagePush) == 32,
			   "GlImagePush must match shaders/image.*");
_Static_assert(offsetof(GlImagePush, screen) == 16,
			   "GlImagePush layout drifted");

/* The uniform buffer behind every `Push` block: the largest of them. */
#define PUSH_BYTES 32

/* The device object behind FnlImage::gpu: this context's texture, made when
 * the image is first drawn and freed by gl_image_free() (and, with the
 * context, by gl_destroy_overlay()). */
typedef struct {
	unsigned tex;
} GlImageObj;

/* ---- programs ------------------------------------------------------------ */

static unsigned compile(GLenum stage, const char *name, const char *src)
{
	GLuint s = gl.CreateShader(stage);
	gl.ShaderSource(s, 1, &src, NULL);
	gl.CompileShader(s);
	GLint ok = GL_FALSE;
	gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		gl.GetShaderInfoLog(s, (GLsizei)sizeof log, NULL, log);
		fnl_log(FNL_LOG_ERROR, "the %s %s shader did not compile: %s", name,
				stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		gl.DeleteShader(s);
		return 0;
	}
	return s;
}

unsigned gl_program(const char *name, const char *vert, const char *frag)
{
	GLuint vs = compile(GL_VERTEX_SHADER, name, vert);
	GLuint fs = compile(GL_FRAGMENT_SHADER, name, frag);
	if (!vs || !fs) {
		gl.DeleteShader(vs);
		gl.DeleteShader(fs);
		return 0;
	}
	GLuint p = gl.CreateProgram();
	gl.AttachShader(p, vs);
	gl.AttachShader(p, fs);
	gl.LinkProgram(p);
	gl.DetachShader(p, vs);
	gl.DetachShader(p, fs);
	gl.DeleteShader(vs);
	gl.DeleteShader(fs);

	GLint ok = GL_FALSE;
	gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024];
		gl.GetProgramInfoLog(p, (GLsizei)sizeof log, NULL, log);
		fnl_log(FNL_LOG_ERROR, "the %s program did not link: %s", name, log);
		gl.DeleteProgram(p);
		return 0;
	}
	/* By name: the push block has no binding to carry over from Vulkan. A
	 * program without one -- the fetch -- simply has no such block. */
	const GLuint block = gl.GetUniformBlockIndex(p, "Push");
	if (block != GL_INVALID_INDEX)
		gl.UniformBlockBinding(p, block, 0);
	return p;
}

void gl_push(App *a, uint32_t slot, const void *data, size_t bytes)
{
	/* The frame being recorded owns this block: a slot's draws are guarded by
	 * the slot's fence, so nothing in flight reads what the next turn writes
	 * into the other slot's. */
	const unsigned buf = GLS(a)->push[slot];
	FNL_ASSERT(bytes <= PUSH_BYTES);
	if (!buf)
		return; /* the overlay did not build; nothing draws either */
	gl.NamedBufferSubData(buf, 0, (GLsizeiptr)bytes, data);
}

/* One instance-rate layout: `n` vec4 attributes at locations 0.. from the
 * offsets given, all from binding 0, which a flush points at the arena. */
static unsigned make_vao(const size_t *offsets, int n)
{
	GLuint v = 0;
	gl.CreateVertexArrays(1, &v);
	for (int i = 0; i < n; i++) {
		gl.EnableVertexArrayAttrib(v, (GLuint)i);
		gl.VertexArrayAttribFormat(v, (GLuint)i, 4, GL_FLOAT, GL_FALSE,
								   (GLuint)offsets[i]);
		gl.VertexArrayAttribBinding(v, (GLuint)i, 0);
	}
	gl.VertexArrayBindingDivisor(v, 0, 1);
	return v;
}

static unsigned make_sampler(GLenum filter)
{
	GLuint s = 0;
	gl.CreateSamplers(1, &s);
	gl.SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, (GLint)filter);
	gl.SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, (GLint)filter);
	gl.SamplerParameteri(s, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.SamplerParameteri(s, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return s;
}

/* ---- the atlas ----------------------------------------------------------- */

static void destroy_atlas(App *a)
{
	if (GLS(a)->font_tex)
		gl.DeleteTextures(1, &GLS(a)->font_tex);
	GLS(a)->font_tex = 0;
	/* And the index text_draw() tests before appending a glyph, so a scale
	 * change whose upload fails appends nothing. */
	a->font_si = -1;
}

/* Decoded into a scratch block and uploaded, as the Vulkan renderer's
 * build_atlas() does; one byte of coverage a texel, row 0 the first row of
 * the bake, which a glyph's uv reads with v = 0 at its top. */
static FnlResult build_atlas(App *a, int si)
{
	const int aw       = (int)FONT_ATLAS_W[si];
	const int ah       = (int)FONT_ATLAS_H[si];
	const size_t bytes = (size_t)aw * (size_t)ah;

	unsigned char *scratch = calloc(1, bytes);
	if (!scratch)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory expanding the %dx%d glyph atlas", aw,
						ah);
	if (!font_decode(font_atlas_blob + FONT_ATLAS_OFF[si], FONT_ATLAS_LEN[si],
					 scratch, bytes)) {
		free(scratch);
		return fnl_fail(FNL_ERR_SYSTEM,
						"the baked atlas for scale %d does not decode to "
						"%dx%d",
						FONT_SCALE[si], aw, ah);
	}

	gl_clear_errors();
	gl.CreateTextures(GL_TEXTURE_2D, 1, &GLS(a)->font_tex);
	gl.TextureStorage2D(GLS(a)->font_tex, 1, GL_R8, aw, ah);
	/* The unpack state is the context's, and on_record may have left any of
	 * it set. */
	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	gl.TextureSubImage2D(GLS(a)->font_tex, 0, 0, 0, aw, ah, GL_RED,
						 GL_UNSIGNED_BYTE, scratch);
	free(scratch);
	if (gl.GetError() != GL_NO_ERROR) {
		destroy_atlas(a);
		return fnl_fail(FNL_ERR_SYSTEM,
						"could not upload the %dx%d glyph atlas", aw, ah);
	}

	a->font_si      = si;
	a->font_atlas_w = aw;
	a->font_atlas_h = ah;
	return FNL_OK;
}

/* Render thread, when it latches a new scale. No drain: a frame in flight
 * that samples the old texture keeps it until it is done. Compared by atlas
 * index, as on Vulkan. */
FnlResult gl_text_set_scale(App *a, int s120)
{
	if (!GLS(a)->prog_text)
		return FNL_OK;
	const int si = scale_index(s120);
	if (si == a->font_si)
		return FNL_OK;
	destroy_atlas(a);
	return build_atlas(a, si);
}

/* ---- building and tearing down ------------------------------------------- */

/* One arena of `bytes` per frame slot, mapped for good: the core appends
 * straight into it, and the slot's fence (frame.c) says when the GPU is done
 * reading it. Coherent, so nothing has to be flushed between the append and
 * the draw. Under 4.1, which has no persistent mapping, the core appends
 * into client memory instead and each flush uploads what it draws
 * (gl_arena_upload()), into a buffer of the slot's that the slot's fence
 * guards as before. */
static FnlResult make_arenas(App *a, unsigned *bufs, size_t bytes, void **maps,
							 void **block)
{
	const GLbitfield f =
		GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
	/* The client memory is one block for every slot, kept whole in the
	 * renderer's state and handed out by offset: a heap pointer stored
	 * inside the loop reads as a leak to -fanalyzer (CLAUDE.md). */
	if (GLS(a)->arenas_client) {
		if (bytes > SIZE_MAX / MAX_FRAMES_IN_FLIGHT)
			return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
							"a %zu-byte overlay arena is too large", bytes);
		*block = malloc(bytes * MAX_FRAMES_IN_FLIGHT);
		if (!*block)
			return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
							"out of memory for a %zu-byte overlay arena",
							bytes);
	}
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		gl.CreateBuffers(1, &bufs[i]);
		if (GLS(a)->arenas_client) {
			gl.NamedBufferStorage(bufs[i], (GLsizeiptr)bytes, NULL, 0);
			maps[i] = (unsigned char *)*block + (size_t)i * bytes;
		} else {
			gl.NamedBufferStorage(bufs[i], (GLsizeiptr)bytes, NULL, f);
			maps[i] = gl.MapNamedBufferRange(bufs[i], 0, (GLsizeiptr)bytes, f);
		}
		if (!maps[i])
			return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
							"could not map a %zu-byte overlay arena", bytes);
	}
	return FNL_OK;
}

static void destroy_arenas(unsigned *bufs, void **maps, void **block)
{
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (bufs[i])
			gl.DeleteBuffers(1, &bufs[i]); /* unmaps it too */
		bufs[i] = 0;
		maps[i] = NULL;
	}
	free(*block);
	*block = NULL;
}

FnlResult gl_create_overlay(App *a)
{
	struct RendererState *g = GLS(a);

	/* The profile's translation of each stage. */
#define SRC(stem) (gl_profile41 ? stem##_glsl41 : stem##_glsl)
	g->prog_rect  = gl_program("rectangle", SRC(rect_vert), SRC(rect_frag));
	g->prog_shape = gl_program("stroke", SRC(shape_vert), SRC(shape_frag));
	/* The subpixel variant always: dual-source blending is core since 3.3,
	 * where Vulkan has it as an optional feature. */
	g->prog_text  = gl_program("text", SRC(text_vert), SRC(text_lcd_frag));
	g->prog_blit  = gl_program("fetch", SRC(fullscreen_vert), SRC(blit_frag));
	g->prog_mlaa  = gl_program("MLAA", SRC(fullscreen_vert), SRC(mlaa_frag));
	g->prog_taau = gl_program("TAAU", SRC(fullscreen_vert), SRC(taau_frag));
	/* The resolve reads three images, and the 4.1 translation loses the
	 * `layout(binding = N)` the 4.50 one keeps: an unset sampler reads unit 0,
	 * which would make the history the scene and the mix a still. Set by name,
	 * which both profiles keep. */
	if (g->prog_taau) {
		gl.UseProgram(g->prog_taau);
		gl.Uniform1i(gl.GetUniformLocation(g->prog_taau, "u_scene"), 0);
		gl.Uniform1i(gl.GetUniformLocation(g->prog_taau, "u_history"), 1);
		gl.Uniform1i(gl.GetUniformLocation(g->prog_taau, "u_depth"), 2);
	}
	g->prog_image = gl_program("image", SRC(image_vert), SRC(image_frag));
#undef SRC
	if (!g->prog_rect || !g->prog_shape || !g->prog_text || !g->prog_blit ||
		!g->prog_mlaa || !g->prog_taau || !g->prog_image)
		return fnl_fail(FNL_ERR_SYSTEM, "the overlay's programs did not build");

	/* One block a slot, and a name refused is no overlay: gl_own_state() binds
	 * it and every push writes into it, so a nought would silently draw
	 * nothing. */
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		gl_clear_errors();
		gl.CreateBuffers(1, &g->push[i]);
		gl.NamedBufferStorage(g->push[i], PUSH_BYTES, NULL,
							  GL_DYNAMIC_STORAGE_BIT);
		/* The name is no store: a refused storage leaves one that every
		 * push would write into and no draw could read. */
		if (!g->push[i] || gl.GetError() != GL_NO_ERROR)
			return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
							"out of memory for the overlay's "
							"push block");
	}
	/* Asked once, as a constant of the context, not at every frame. */
	GLint clips = 0;
	gl.GetIntegerv(GL_MAX_CLIP_DISTANCES, &clips);
	g->clip_distances = clips;

	const size_t rect[2]  = { offsetof(RectInstance, rect),
							  offsetof(RectInstance, color) };
	const size_t shape[5] = { offsetof(ShapeInstance, p0),
							  offsetof(ShapeInstance, p2),
							  offsetof(ShapeInstance, color),
							  offsetof(ShapeInstance, color2),
							  offsetof(ShapeInstance, shape_kind) };
	const size_t text[3]  = { offsetof(GlyphInstance, rect),
							  offsetof(GlyphInstance, uv),
							  offsetof(GlyphInstance, color) };
	g->vao_rect           = make_vao(rect, 2);
	g->vao_shape          = make_vao(shape, 5);
	g->vao_text           = make_vao(text, 3);
	gl.CreateVertexArrays(1, &g->vao_empty);

	g->sampler_nearest = make_sampler(GL_NEAREST);
	g->sampler_linear  = make_sampler(GL_LINEAR);
	/* A nought here is a layout bound as name 0 -- GL_INVALID_OPERATION,
	 * or a sampler-less texture -- so the build fails rather than draw
	 * without them. */
	if (!g->vao_rect || !g->vao_shape || !g->vao_text || !g->vao_empty ||
		!g->sampler_nearest || !g->sampler_linear)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory for the overlay's layouts and "
						"samplers");

	g->arenas_client = gl_profile41;
	FNL_TRY(make_arenas(a, g->text_buf, TEXT_ARENA_BYTES, (void **)a->text_map,
						&g->text_block));
	FNL_TRY(make_arenas(a, g->ovl_buf, OVERLAY_ARENA_BYTES, (void **)a->ovl_map,
						&g->ovl_block));

	/* The scale is 120 until the render thread latches another, as on
	 * Vulkan. */
	a->font_si = -1;
	return build_atlas(a, scale_index(scale120(a)));
}

void gl_destroy_overlay(App *a)
{
	struct RendererState *g = GLS(a);
	destroy_atlas(a);
	if (g->cover_tex)
		gl.DeleteTextures(1, &g->cover_tex);
	g->cover_tex = 0;
	g->cover_gen = 0;
	destroy_arenas(g->text_buf, (void **)a->text_map, &g->text_block);
	destroy_arenas(g->ovl_buf, (void **)a->ovl_map, &g->ovl_block);
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (g->fence[i])
			gl.DeleteSync((GLsync)g->fence[i]);
		g->fence[i] = NULL;
	}
	/* The caller's images first: their textures are the context's, and the
	 * device below is what takes them (gl_image_free() is the same, one).
	 * Each is left with a null gpu, so a rebuilt context uploads the copy
	 * again. */
	for (FnlImage *img = a->images; img; img = img->next) {
		GlImageObj *o = img->gpu;
		if (o) {
			gl.DeleteTextures(1, &o->tex);
			free(o);
			img->gpu = NULL;
		}
	}
	const unsigned progs[7] = { g->prog_rect, g->prog_shape, g->prog_text,
								g->prog_blit, g->prog_mlaa,  g->prog_taau,
								g->prog_image };
	for (int i = 0; i < 7; i++)
		if (progs[i])
			gl.DeleteProgram(progs[i]);
	const unsigned vaos[4] = { g->vao_rect, g->vao_shape, g->vao_text,
							   g->vao_empty };
	gl.DeleteVertexArrays(4, vaos);
	const unsigned samplers[2] = { g->sampler_nearest, g->sampler_linear };
	gl.DeleteSamplers(2, samplers);
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		if (g->push[i])
			gl.DeleteBuffers(1, &g->push[i]);

	g->prog_rect = g->prog_shape = g->prog_text = 0;
	g->prog_blit = g->prog_mlaa = g->prog_taau = g->prog_image = 0;
	g->vao_rect = g->vao_shape = g->vao_text = g->vao_empty = 0;
	g->sampler_nearest = g->sampler_linear = 0;
	memset(g->push, 0, sizeof g->push);
}

/* ---- drawing ------------------------------------------------------------- */

void gl_own_state(App *a)
{
	gl.Disable(GL_DEPTH_TEST);
	gl.Disable(GL_STENCIL_TEST);
	gl.Disable(GL_CULL_FACE);
	gl.Disable(GL_BLEND);
	gl.Disable(GL_RASTERIZER_DISCARD);
	gl.Disable(GL_PRIMITIVE_RESTART);
	gl.Disable(GL_POLYGON_OFFSET_FILL);
	gl.Disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	gl.Disable(GL_COLOR_LOGIC_OP);
	/* The overlay's shaders write no gl_ClipDistance, and a distance left
	 * enabled reads an undefined one: clipped where a caller's scene
	 * enabled one and did not turn it off. */
	for (int i = 0; i < GLS(a)->clip_distances; i++)
		gl.Disable((GLenum)(GL_CLIP_DISTANCE0 + i));
	gl.Enable(GL_SCISSOR_TEST);
	gl.Enable(GL_FRAMEBUFFER_SRGB);
	gl.PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	gl.DepthMask(GL_TRUE);
	gl.BlendEquation(GL_FUNC_ADD);
	/* The slot's, as every push writes into it: the frame being recorded is
	 * the one whose draws may still be reading the other. App::frame is the
	 * slot, reduced as draw() reduces it; overlay_begin() has not run yet,
	 * so App::ovl_img still holds the previous frame's -- another renderer's
	 * image index after a swap, past this array's end. */
	gl.BindBufferBase(GL_UNIFORM_BUFFER, 0,
					  GLS(a)->push[a->frame % MAX_FRAMES_IN_FLIGHT]);
}

/* The blend each kind is drawn with: rectangles replace what is under them,
 * strokes blend on their alpha, text on a weight per channel. The same
 * factors as the Vulkan renderer's pipelines (make_overlay_pipeline()). */
typedef enum { BLEND_NONE, BLEND_ALPHA, BLEND_DUAL_SRC } Blend;

static void set_blend(Blend b)
{
	if (b == BLEND_NONE) {
		gl.Disable(GL_BLEND);
		return;
	}
	gl.Enable(GL_BLEND);
	if (b == BLEND_DUAL_SRC)
		gl.BlendFuncSeparate(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR,
							 GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA);
	else
		gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
							 GL_ONE_MINUS_SRC_ALPHA);
}

/* The library's GL state, re-established for a deferred draw: a caller may
 * draw with the context inside on_overlay (fnl_gl.h), and whatever it leaves
 * -- its framebuffer, viewport, masks, a clip distance, the blend or sRGB
 * state -- would otherwise reach these draws, which are issued after the
 * callback returns. The scissor's rectangle is not touched: the core's
 * overlay_scissor() sets it, and gl_own_state() only enables the test. */
static void overlay_own_state(App *a)
{
	gl_own_state(a);
	gl.BindFramebuffer(GL_FRAMEBUFFER, GLS(a)->window.fbo);
	gl.Viewport(0, 0, a->extent.w, a->extent.h);
}

/* Draw the instances appended since this kind was last flushed: the arena
 * bound at the kind's offset and the first instance where the last flush
 * stopped, so a second flush reads on with nothing copied, as on Vulkan. */
static void flush(App *a, unsigned prog, unsigned vao, size_t offset,
				  size_t stride, Blend blend, const uint32_t *count,
				  uint32_t *drawn)
{
	if (*count == *drawn || !GLS(a)->overlay_on || !prog)
		return;
	overlay_own_state(a);
	if (GLS(a)->arenas_client)
		gl_arena_upload(GLS(a)->ovl_buf[a->ovl_img], a->ovl_map[a->ovl_img],
						offset + (size_t)*drawn * stride,
						(size_t)(*count - *drawn) * stride);
	gl.UseProgram(prog);
	gl.BindVertexArray(vao);
	gl.VertexArrayVertexBuffer(vao, 0, GLS(a)->ovl_buf[a->ovl_img],
							   (GLintptr)offset, (GLsizei)stride);
	set_blend(blend);
	const GlOverlayPush push = { .screen = { (float)a->extent.w,
											 (float)a->extent.h } };
	gl_push(a, a->ovl_img, &push, sizeof push);
	gl.DrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4,
									   (GLsizei)(*count - *drawn), *drawn);
	*drawn = *count;
}

void gl_overlay_flush_rects(App *a)
{
	flush(a, GLS(a)->prog_rect, GLS(a)->vao_rect, 0, sizeof(RectInstance),
		  BLEND_NONE, &a->rect_count, &a->rect_drawn);
}

void gl_overlay_flush_shapes(App *a)
{
	flush(a, GLS(a)->prog_shape, GLS(a)->vao_shape, OVERLAY_SHAPE_OFF,
		  sizeof(ShapeInstance), BLEND_ALPHA, &a->shape_count, &a->shape_drawn);
}

/* NULL restores the whole image. The image's row 0 is its top, so a
 * rectangle in the overlay's pixels is a scissor as it stands. */
void gl_overlay_scissor(App *a, const FnlRect *r)
{
	if (!GLS(a)->overlay_on)
		return;
	if (r)
		gl.Scissor(r->x, r->y, r->w, r->h);
	else
		gl.Scissor(0, 0, a->extent.w, a->extent.h);
}

/* ---- images --------------------------------------------------------------- */

bool gl_image_supported(const App *a)
{
	return a->renderer_state != NULL && GLS(a)->prog_image != 0;
}

/* The context's texture from the copy the library kept. False where anything
 * failed, with *out left null: the draw is skipped and the caller's picture
 * does not appear, which is what fnl_image_supported() promised. */
static bool image_upload(App *a, FnlImage *img, GlImageObj **out)
{
	*out          = NULL;
	GlImageObj *o = calloc(1, sizeof *o);
	if (!o)
		return false;

	/* _SRGB where the window is, as the cover: GL_FRAMEBUFFER_SRGB is on, so
	 * the sampler decodes each texel to linear before the blend and the
	 * framebuffer re-encodes on write. */
	gl_clear_errors();
	gl.CreateTextures(GL_TEXTURE_2D, 1, &o->tex);
	gl.TextureStorage2D(o->tex, 1,
						a->linear_target ? GL_SRGB8_ALPHA8 : GL_RGBA8,
						img->w, img->h);
	/* The unpack state is the context's, and on_record may have left any of
	 * it set. */
	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	gl.TextureSubImage2D(o->tex, 0, 0, 0, img->w, img->h, GL_RGBA,
						 GL_UNSIGNED_BYTE, img->pixels);
	if (gl.GetError() != GL_NO_ERROR) {
		gl.DeleteTextures(1, &o->tex);
		free(o);
		return false;
	}
	*out = o;
	return true;
}

void gl_overlay_image(App *a, FnlImage *img, int x, int y, int w, int h)
{
	struct RendererState *g = GLS(a);
	if (!g->overlay_on || !g->prog_image)
		return;
	overlay_own_state(a);

	if (!img->gpu) {
		GlImageObj *o = NULL;
		if (!image_upload(a, img, &o)) {
			fnl_log(FNL_LOG_WARN,
					"the %dx%d image would not upload; it is "
					"not drawn",
					img->w, img->h);
			return;
		}
		img->gpu = o;
	}

	/* What was appended before it is behind it: the core flushed the
	 * rectangles and strokes before this call, so the image lands between
	 * them as it did in call order. */
	gl.UseProgram(g->prog_image);
	gl.BindVertexArray(g->vao_empty);
	gl.BindTextureUnit(0, ((GlImageObj *)img->gpu)->tex);
	gl.BindSampler(0, g->sampler_linear);
	set_blend(BLEND_ALPHA);
	const GlImagePush push = {
		.rect   = { (float)x, (float)y, (float)w, (float)h },
		.screen = { (float)a->extent.w, (float)a->extent.h },
	};
	gl_push(a, a->ovl_img, &push, sizeof push);
	gl.DrawArrays(GL_TRIANGLES, 0, 6);
}

void gl_image_free(App *a, FnlImage *img)
{
	/* The thread is the image's window's from here: fnl_image_destroy() may
	 * run on the main thread, at a teardown that has already destroyed
	 * another window's renderer and so unbound the thread. */
	if (!GLS(a)->loaded) {
		/* No context's entry points to delete through, and every name a
		 * destroyed context had is free anyway: the object goes, and the
		 * image is left to upload again. */
		free(img->gpu);
		img->gpu = NULL;
		return;
	}
	gl_bind(a);
	GlImageObj *o = img->gpu;
	if (!o)
		return;
	img->gpu = NULL;
	gl.DeleteTextures(1, &o->tex);
	free(o);
}

/* Every caller image's object, with the context that made it gone and none
 * makeable: no delete is run -- the context's destroy freed every name --
 * and each image keeps its pixels, uploading them again on the next device.
 * Without this a later success would bind names out of the dead context and
 * a destroy would delete a live texture of the new one. */
void gl_images_forget(App *a)
{
	for (FnlImage *img = a->images; img; img = img->next)
		if (img->gpu) {
			free(img->gpu);
			img->gpu = NULL;
		}
}

/* ---- the cover ----------------------------------------------------------- */

/* App::snap into a texture of this context: BGRA on upload, the
 * snapshot's order (app_snapshot_set()), and an sRGB format where the
 * window's image is one, so the bytes round-trip through the sampler and
 * GL_FRAMEBUFFER_SRGB unchanged. No drain for the one before: a frame in
 * flight that samples it keeps it until it is done. */
static bool cover_upload(App *a)
{
	struct RendererState *g = GLS(a);
	if (g->cover_tex)
		gl.DeleteTextures(1, &g->cover_tex);
	g->cover_tex = 0;
	gl_clear_errors();
	gl.CreateTextures(GL_TEXTURE_2D, 1, &g->cover_tex);
	gl.TextureStorage2D(g->cover_tex, 1,
						a->linear_target ? GL_SRGB8_ALPHA8 : GL_RGBA8,
						a->snap_w, a->snap_h);
	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	gl.TextureSubImage2D(g->cover_tex, 0, 0, 0, a->snap_w, a->snap_h, GL_BGRA,
						 GL_UNSIGNED_BYTE, a->snap_px);
	if (gl.GetError() != GL_NO_ERROR) {
		gl.DeleteTextures(1, &g->cover_tex);
		g->cover_tex = 0;
		return false;
	}
	return true;
}

/* The cover over the window at `alpha`: the fetch's program over the
 * covering triangle, blended by a constant, the alpha channel left as it
 * is. Uploaded once per snapshot (App::snap_gen) and tried once. */
bool gl_overlay_cover(App *a, float alpha)
{
	struct RendererState *g = GLS(a);
	if (!g->overlay_on || !a->snap_valid || !g->prog_blit)
		return false;
	overlay_own_state(a);
	if (g->cover_gen != a->snap_gen) {
		g->cover_gen = a->snap_gen;
		if (!cover_upload(a))
			fnl_log(FNL_LOG_WARN,
					"the frame read back before the move "
					"would not upload; the move dissolves from black");
	}
	if (!g->cover_tex)
		return false;
	gl.UseProgram(g->prog_blit);
	gl.BindVertexArray(g->vao_empty);
	gl.BindTextureUnit(0, g->cover_tex);
	gl.BindSampler(0, g->sampler_linear);
	gl.Enable(GL_BLEND);
	gl.BlendColor(0.0f, 0.0f, 0.0f, alpha);
	gl.BlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA,
						 GL_ZERO, GL_ONE);
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
	return true;
}

/* The seam's cover_release. No drain, as cover_upload() needs none, and
 * the snapshot's readback is client memory read synchronously. Kept under
 * a surfaceless bridge, whose present reads into it on any frame the pack
 * buffer cannot serve; freed and grown again there, it would be an
 * allocation a frame. */
void gl_cover_release(App *a)
{
	struct RendererState *g = GLS(a);
	if (g->cover_tex)
		gl.DeleteTextures(1, &g->cover_tex);
	g->cover_tex = 0;
	if (g->readback && !gl_bridge_surfaceless(a)) {
		free(g->readback);
		g->readback       = NULL;
		g->readback_bytes = 0;
	}
}

FnlResult gl_text_end(App *a)
{
	if (a->text_count == a->text_drawn || !GLS(a)->overlay_on ||
		!GLS(a)->prog_text || !GLS(a)->font_tex)
		return FNL_OK;
	overlay_own_state(a);

	if (GLS(a)->arenas_client)
		gl_arena_upload(GLS(a)->text_buf[a->text_img], a->text_map[a->text_img],
						(size_t)a->text_drawn * sizeof(GlyphInstance),
						(size_t)(a->text_count - a->text_drawn) *
							sizeof(GlyphInstance));
	gl.UseProgram(GLS(a)->prog_text);
	gl.BindVertexArray(GLS(a)->vao_text);
	gl.VertexArrayVertexBuffer(GLS(a)->vao_text, 0,
							   GLS(a)->text_buf[a->text_img], 0,
							   (GLsizei)sizeof(GlyphInstance));
	gl.BindTextureUnit(0, GLS(a)->font_tex);
	gl.BindSampler(0, GLS(a)->sampler_nearest);
	set_blend(BLEND_DUAL_SRC);

	/* As on Vulkan: the blend space, and the width of one atlas texel, which
	 * is zero -- the grayscale answer -- for a resampled scale. */
	const GlTextPush push = {
		.screen  = { (float)a->extent.w, (float)a->extent.h },
		.srgb    = a->linear_target ? 1u : 0u,
		.texel_u = text_resampled(a) || a->font_atlas_w <= 0
					   ? 0.0f
					   : 1.0f / (float)a->font_atlas_w,
	};
	gl_push(a, a->text_img, &push, sizeof push);
	gl.DrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4,
									   (GLsizei)(a->text_count - a->text_drawn),
									   a->text_drawn);
	a->text_drawn = a->text_count;
	return FNL_OK;
}
