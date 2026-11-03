/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The test pattern, as a Fenestal client draws it with OpenGL: two programs and
 * two draws, from the same shaders scene_vulkan.c builds its Vulkan pipelines
 * from, translated for GL by spirv-cross at build time.
 *
 * As in scene_vulkan.c, this file does not know whether the scene is drawn
 * straight into the window or into a smaller image, whether it is multisampled,
 * or how the frame is paced. Unlike there, one program serves every target --
 * GL has no render-pass compatibility -- and the frame's state is the caller's
 * to set on every call, since GL keeps one state machine for everything the
 * context draws.
 *
 * Written in the 4.1 subset, which a 4.5 context runs too, so that the same
 * calls serve both: a context is 4.1 where the device makes no 4.5
 * (fnl_gl_version()), and the shaders are then their 4.10 translation,
 * which maps depth into GL's range itself.
 */

#include "demo.h"

#include <fenestal/fnl_gl.h>

#include <GL/glcorearb.h>

#include <stdio.h>
#include <string.h>

#include "scene.vert.gl.h"
#include "scene.frag.gl.h"
#include "star.vert.gl.h"
#include "star.frag.gl.h"
#include "scene.vert.gl41.h"
#include "scene.frag.gl41.h"
#include "star.vert.gl41.h"
#include "star.frag.gl41.h"

/* As in scene_vulkan.c: 36 lit wedges, two triangles each. */
#define STAR_VERTICES (36 * 2 * 3)

/* What the shaders' `Push` block holds, as std140 lays it out: the same
 * bytes as scene_vulkan.c's push constant, and pinned the same way. */
typedef struct {
	float    size[2];
	float    time;
	uint32_t srgb;
} ScenePush;

_Static_assert(sizeof(ScenePush) == 16, "ScenePush must match scene.frag");
_Static_assert(offsetof(ScenePush, time) == 8, "ScenePush layout drifted");
_Static_assert(offsetof(ScenePush, srgb) == 12, "ScenePush layout drifted");

/* The entry points this file calls, loaded through fnl_gl_proc() at every
 * on_gpu_created: a loss may land on another driver. */
#define SCENE_GL(X)                                                           \
	X(PFNGLCREATESHADERPROC,          CreateShader)                           \
	X(PFNGLSHADERSOURCEPROC,          ShaderSource)                           \
	X(PFNGLCOMPILESHADERPROC,         CompileShader)                          \
	X(PFNGLGETSHADERIVPROC,           GetShaderiv)                            \
	X(PFNGLGETSHADERINFOLOGPROC,      GetShaderInfoLog)                       \
	X(PFNGLDELETESHADERPROC,          DeleteShader)                           \
	X(PFNGLCREATEPROGRAMPROC,         CreateProgram)                          \
	X(PFNGLATTACHSHADERPROC,          AttachShader)                           \
	X(PFNGLLINKPROGRAMPROC,           LinkProgram)                            \
	X(PFNGLGETPROGRAMIVPROC,          GetProgramiv)                           \
	X(PFNGLDELETEPROGRAMPROC,         DeleteProgram)                          \
	X(PFNGLUSEPROGRAMPROC,            UseProgram)                             \
	X(PFNGLGETUNIFORMBLOCKINDEXPROC,  GetUniformBlockIndex)                   \
	X(PFNGLUNIFORMBLOCKBINDINGPROC,   UniformBlockBinding)                    \
	X(PFNGLGENBUFFERSPROC,            GenBuffers)                             \
	X(PFNGLBINDBUFFERPROC,            BindBuffer)                             \
	X(PFNGLBUFFERDATAPROC,            BufferData)                             \
	X(PFNGLBUFFERSUBDATAPROC,         BufferSubData)                          \
	X(PFNGLBINDBUFFERBASEPROC,        BindBufferBase)                         \
	X(PFNGLDELETEBUFFERSPROC,         DeleteBuffers)                          \
	X(PFNGLGENVERTEXARRAYSPROC,       GenVertexArrays)                        \
	X(PFNGLBINDVERTEXARRAYPROC,       BindVertexArray)                        \
	X(PFNGLDELETEVERTEXARRAYSPROC,    DeleteVertexArrays)                     \
	X(PFNGLDRAWARRAYSPROC,            DrawArrays)                             \
	X(PFNGLDISABLEPROC,               Disable)                                \
	X(PFNGLGETSTRINGPROC,             GetString)

typedef struct {
#define SCENE_MEMBER(type, name) type name;
	SCENE_GL(SCENE_MEMBER)
#undef SCENE_MEMBER
	GLuint pattern;     /* 0 where it did not build: nothing is drawn */
	GLuint star;
	GLuint vao;         /* empty: both shaders build geometry from the index */
	GLuint push;
	bool   srgb;
	bool   loaded;
} Scene;

static Scene g_scene;

/* The block the shaders' `Push` is bound to here. The library binds its
 * own buffer to point 0 before every draw of its own, so any point will do;
 * 1 keeps the two apart for anyone reading a GL trace. */
#define PUSH_BINDING 1

static GLuint compile(Scene *s, GLenum stage, const char *src)
{
	GLuint sh = s->CreateShader(stage);
	s->ShaderSource(sh, 1, &src, NULL);
	s->CompileShader(sh);
	GLint ok = GL_FALSE;
	s->GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		s->GetShaderInfoLog(sh, (GLsizei)sizeof log, NULL, log);
		fprintf(stderr, "a scene shader did not compile: %s\n", log);
		s->DeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint make_program(Scene *s, const char *vert, const char *frag)
{
	GLuint vs = compile(s, GL_VERTEX_SHADER, vert);
	GLuint fs = compile(s, GL_FRAGMENT_SHADER, frag);
	GLuint p = 0;
	if (vs && fs) {
		p = s->CreateProgram();
		s->AttachShader(p, vs);
		s->AttachShader(p, fs);
		s->LinkProgram(p);
		GLint ok = GL_FALSE;
		s->GetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) {
			fprintf(stderr, "a scene program did not link\n");
			s->DeleteProgram(p);
			p = 0;
		}
	}
	s->DeleteShader(vs);
	s->DeleteShader(fs);
	if (p) {
		const GLuint block = s->GetUniformBlockIndex(p, "Push");
		if (block != GL_INVALID_INDEX)
			s->UniformBlockBinding(p, block, PUSH_BINDING);
	}
	return p;
}

void scene_gl_on_gpu_created(void *ud, FnlApp *app)
{
	(void)ud;
	Scene *s = &g_scene;
	memset(s, 0, sizeof *s);

	if (fnl_renderer(app) != FNL_RENDERER_OPENGL) {
		fprintf(stderr, "the running renderer is not OpenGL; "
						"no scene will be drawn\n");
		return;
	}
#define SCENE_LOAD(type, name)                                                \
	if (!(s->name = (type)fnl_gl_proc(app, "gl" #name))) {                    \
		fprintf(stderr, "the driver has no gl" #name "\n");                   \
		return;                                                               \
	}
	SCENE_GL(SCENE_LOAD)
#undef SCENE_LOAD
	s->loaded = true;

	/* Whether the hardware encodes linear -> sRGB as fragments are written,
	 * which decides whether the shader must; read off the format, as
	 * scene_vulkan.c reads it off Vulkan's. */
	FnlGlSceneLayout layout;
	if (fnl_gl_scene_layout(app, FNL_TARGET_DIRECT, &layout))
		s->srgb = layout.color_format == GL_SRGB8_ALPHA8;

	/* The translation for the context's version: 4.10's maps depth
	 * itself, where 4.5 has glClipControl set by the library. */
	const bool v41 = fnl_gl_version(app) == 41;
	s->pattern = make_program(s, v41 ? scene_vert_glsl41 : scene_vert_glsl,
							  v41 ? scene_frag_glsl41 : scene_frag_glsl);
	s->star    = make_program(s, v41 ? star_vert_glsl41 : star_vert_glsl,
							  v41 ? star_frag_glsl41 : star_frag_glsl);
	s->GenVertexArrays(1, &s->vao);
	s->GenBuffers(1, &s->push);
	s->BindBuffer(GL_UNIFORM_BUFFER, s->push);
	s->BufferData(GL_UNIFORM_BUFFER, sizeof(ScenePush), NULL,
				  GL_DYNAMIC_DRAW);
}

void scene_gl_on_gpu_lost(void *ud, FnlApp *app)
{
	(void)ud; (void)app;
	Scene *s = &g_scene;
	if (!s->loaded)
		return;
	s->DeleteProgram(s->pattern);
	s->DeleteProgram(s->star);
	s->DeleteVertexArrays(1, &s->vao);
	s->DeleteBuffers(1, &s->push);
	memset(s, 0, sizeof *s);
}

/* The scene itself, as in scene_vulkan.c: the pattern over one covering
 * triangle, then the star's wedges as primitives FSAA can sample. Into whatever
 * is bound -- the library bound it, with the viewport and scissor set. */
void scene_gl_on_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	(void)ud;
	Scene *s = &g_scene;
	uint32_t fbo;
	if (!s->pattern || !s->star || !fnl_gl_scene_framebuffer(app, &fbo))
		return;

	const ScenePush pc = {
		.size = { (float)f->scene_w_px, (float)f->scene_h_px },
		.time = f->scene_time,
		.srgb = s->srgb ? 1u : 0u,
	};
	/* Bound to its point, which binds the generic target too, and then
	 * written through that. */
	s->BindBufferBase(GL_UNIFORM_BUFFER, PUSH_BINDING, s->push);
	s->BufferSubData(GL_UNIFORM_BUFFER, 0, sizeof pc, &pc);

	/* What scene_vulkan.c's pipelines fix: no depth test, no blending, no
	 * culling. GL's state is the context's, so it is said each time. */
	s->Disable(GL_DEPTH_TEST);
	s->Disable(GL_BLEND);
	s->Disable(GL_CULL_FACE);
	s->BindVertexArray(s->vao);

	s->UseProgram(s->pattern);
	s->DrawArrays(GL_TRIANGLES, 0, 3);
	s->UseProgram(s->star);
	s->DrawArrays(GL_TRIANGLES, 0, STAR_VERTICES);
}


int scene_gl_version(FnlApp *app)
{
	return fnl_gl_version(app);
}

/* ---- the driver --------------------------------------------------------- */

/* GL_VENDOR and GL_RENDERER as the driver's name, GL_VERSION as the API's
 * and its own: Mesa's is "4.6 (Core Profile) Mesa 26.2.5", NVIDIA's
 * "4.6.0 NVIDIA 580.95.05". GL names no PCI ids. */
bool scene_gl_driver(FnlApp *app, DemoDriver *d)
{
	(void)app;
	Scene *s = &g_scene;
	memset(d, 0, sizeof *d);
	if (!s->loaded)
		return false;
	const char *vendor = (const char *)s->GetString(GL_VENDOR);
	const char *renderer = (const char *)s->GetString(GL_RENDERER);
	const char *version = (const char *)s->GetString(GL_VERSION);
	snprintf(d->name, sizeof d->name, "%s", vendor ? vendor : "");
	snprintf(d->info, sizeof d->info, "%s", renderer ? renderer : "");
	snprintf(d->api, sizeof d->api, "OpenGL %s", version ? version : "?");
	return true;
}
