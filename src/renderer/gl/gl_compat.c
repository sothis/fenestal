/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The 4.1 profile's stand-ins for what 4.5 adds (gl_internal.h, FNL_GL_45),
 * each made of calls a 4.1 core context has, so that every call site of the
 * renderer is the same under either profile (DESIGN *OpenGL 4.1*).
 *
 * Direct state access becomes bind-to-edit on binding points nothing else
 * here uses -- a buffer on GL_COPY_WRITE_BUFFER, a texture on unit 0's
 * GL_TEXTURE_2D, a renderbuffer on GL_RENDERBUFFER -- and on the draw
 * framebuffer, which is saved and put back, since the frame binds it and
 * expects it bound. No vertex array's state is touched but the one being
 * drawn with. Every texture the renderer makes is a GL_TEXTURE_2D, which is
 * the target the texture stand-ins bind; a DSA call names no target.
 *
 * A vertex array's format, binding and buffer (4.3's separate format) are
 * kept here, per vertex array, and given to glVertexAttribPointer at the
 * draw, which is also where the base instance goes (4.2): each instanced
 * attribute pointed at the draw's first instance, drawn from 0. Render
 * thread only, as every GL call of the renderer's.
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"

/* The vertex arrays the stand-ins keep an account of: four are made per
 * device (overlay.c), and a lost context's, never deleted, are taken over
 * by name when its successor hands the same names out. */
#define VAO_RECORDS 16
#define VAO_ATTRIBS 4
#define VAO_BINDINGS 1

typedef struct {
	bool      on;
	GLint     size;
	GLenum    type;
	GLboolean normalized;
	GLuint    offset;
	GLuint    binding;
} AttribRecord;

typedef struct {
	GLuint     buffer;
	GLintptr   offset;
	GLsizei    stride;
	GLuint     divisor;
} BindingRecord;

typedef struct {
	GLuint        name;
	AttribRecord  attr[VAO_ATTRIBS];
	BindingRecord bind[VAO_BINDINGS];
} VaoRecord;

static VaoRecord vaos[VAO_RECORDS];
static PFNGLDELETEVERTEXARRAYSPROC real_delete_vertex_arrays;

static VaoRecord *vao_find(GLuint name)
{
	for (int i = 0; i < VAO_RECORDS; i++)
		if (name && vaos[i].name == name)
			return &vaos[i];
	return NULL;
}

/* ---- buffers ------------------------------------------------------------- */

static void APIENTRY create_buffers(GLsizei n, GLuint *b)
{
	gl.GenBuffers(n, b);
	for (GLsizei i = 0; i < n; i++)
		gl.BindBuffer(GL_COPY_WRITE_BUFFER, b[i]);   /* made, as DSA's are */
}

/* Mutable storage of the size asked for, its usage hint read off the
 * flags: read back where it is mapped for reading, edited where it may be
 * written, and otherwise written once a frame. */
static void APIENTRY buffer_storage(GLuint b, GLsizeiptr size,
									const void *data, GLbitfield flags)
{
	const GLenum usage = (flags & GL_MAP_READ_BIT) ? GL_STREAM_READ
					   : (flags & GL_DYNAMIC_STORAGE_BIT) ? GL_DYNAMIC_DRAW
					   : GL_STREAM_DRAW;
	gl.BindBuffer(GL_COPY_WRITE_BUFFER, b);
	gl.BufferData(GL_COPY_WRITE_BUFFER, size, data, usage);
}

/* Persistence and coherence are 4.4's and are dropped: the 4.1 profile
 * never keeps a mapping across a draw (overlay.c's arenas). */
static void *APIENTRY map_buffer_range(GLuint b, GLintptr off,
									   GLsizeiptr len, GLbitfield access)
{
	gl.BindBuffer(GL_COPY_WRITE_BUFFER, b);
	return gl.MapBufferRange(GL_COPY_WRITE_BUFFER, off, len,
							 access & ~(GLbitfield)(GL_MAP_PERSISTENT_BIT |
													GL_MAP_COHERENT_BIT));
}

static GLboolean APIENTRY unmap_buffer(GLuint b)
{
	gl.BindBuffer(GL_COPY_WRITE_BUFFER, b);
	return gl.UnmapBuffer(GL_COPY_WRITE_BUFFER);
}

static void APIENTRY buffer_sub_data(GLuint b, GLintptr off, GLsizeiptr size,
									 const void *data)
{
	gl.BindBuffer(GL_COPY_WRITE_BUFFER, b);
	gl.BufferSubData(GL_COPY_WRITE_BUFFER, off, size, data);
}

/* ---- vertex arrays ------------------------------------------------------- */

static void APIENTRY create_vertex_arrays(GLsizei n, GLuint *v)
{
	gl.GenVertexArrays(n, v);
	for (GLsizei i = 0; i < n; i++) {
		VaoRecord *r = vao_find(v[i]);
		for (int k = 0; !r && k < VAO_RECORDS; k++)
			if (!vaos[k].name)
				r = &vaos[k];
		if (!r) {
			fnl_log(FNL_LOG_ERROR, "OpenGL 4.1: no room to keep vertex "
					"array %u's layout", (unsigned)v[i]);
			continue;
		}
		*r = (VaoRecord){ .name = v[i] };
	}
}

static void APIENTRY delete_vertex_arrays(GLsizei n, const GLuint *v)
{
	for (GLsizei i = 0; i < n; i++) {
		VaoRecord *r = vao_find(v[i]);
		if (r)
			*r = (VaoRecord){ 0 };
	}
	real_delete_vertex_arrays(n, v);
}

static void APIENTRY vertex_buffer(GLuint v, GLuint binding, GLuint buffer,
								   GLintptr offset, GLsizei stride)
{
	VaoRecord *r = vao_find(v);
	if (!r || binding >= VAO_BINDINGS)
		return;
	r->bind[binding].buffer = buffer;
	r->bind[binding].offset = offset;
	r->bind[binding].stride = stride;
}

static void APIENTRY attrib_format(GLuint v, GLuint i, GLint size,
								   GLenum type, GLboolean normalized,
								   GLuint offset)
{
	VaoRecord *r = vao_find(v);
	if (!r || i >= VAO_ATTRIBS)
		return;
	r->attr[i].size = size;
	r->attr[i].type = type;
	r->attr[i].normalized = normalized;
	r->attr[i].offset = offset;
}

static void APIENTRY attrib_binding(GLuint v, GLuint i, GLuint binding)
{
	VaoRecord *r = vao_find(v);
	if (r && i < VAO_ATTRIBS)
		r->attr[i].binding = binding;
}

static void APIENTRY enable_attrib(GLuint v, GLuint i)
{
	VaoRecord *r = vao_find(v);
	if (r && i < VAO_ATTRIBS)
		r->attr[i].on = true;
}

static void APIENTRY binding_divisor(GLuint v, GLuint binding,
									 GLuint divisor)
{
	VaoRecord *r = vao_find(v);
	if (r && binding < VAO_BINDINGS)
		r->bind[binding].divisor = divisor;
}

/* The bound vertex array's attributes pointed at its buffers, each
 * instanced one at `base`'s instance, and the instances drawn from 0. */
static void APIENTRY draw_base_instance(GLenum mode, GLint first,
										GLsizei count, GLsizei instances,
										GLuint base)
{
	GLint bound = 0;
	gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &bound);
	const VaoRecord *r = vao_find((GLuint)bound);
	if (r) {
		for (GLuint i = 0; i < VAO_ATTRIBS; i++) {
			const AttribRecord *at = &r->attr[i];
			if (!at->on || at->binding >= VAO_BINDINGS)
				continue;
			const BindingRecord *b = &r->bind[at->binding];
			const GLintptr skip = b->divisor
				? (GLintptr)(base / b->divisor) * b->stride : 0;
			gl.BindBuffer(GL_ARRAY_BUFFER, b->buffer);
			gl.VertexAttribPointer(i, at->size, at->type, at->normalized,
								   b->stride,
								   (const void *)(uintptr_t)(b->offset +
															 at->offset +
															 skip));
			gl.VertexAttribDivisor(i, b->divisor);
			gl.EnableVertexAttribArray(i);
		}
	}
	gl.DrawArraysInstanced(mode, first, count, instances);
}

/* ---- textures and samplers ----------------------------------------------- */

static void APIENTRY create_textures(GLenum target, GLsizei n, GLuint *t)
{
	gl.GenTextures(n, t);
	gl.ActiveTexture(GL_TEXTURE0);
	for (GLsizei i = 0; i < n; i++)
		gl.BindTexture(target, t[i]);
}

/* The client format a level is specified with, for a level made with no
 * pixels: any that the internal format takes. */
static void pixel_format(GLenum internal, GLenum *format, GLenum *type)
{
	switch (internal) {
	case GL_R8:
		*format = GL_RED;
		*type = GL_UNSIGNED_BYTE;
		break;
	case GL_DEPTH_COMPONENT32F:
		*format = GL_DEPTH_COMPONENT;
		*type = GL_FLOAT;
		break;
	default:
		*format = GL_RGBA;
		*type = GL_UNSIGNED_BYTE;
		break;
	}
}

/* Every level specified with no pixels, and the levels past them cut off,
 * which is what makes the texture complete as immutable storage is. */
static void APIENTRY texture_storage(GLuint t, GLsizei levels,
									 GLenum internal, GLsizei w, GLsizei h)
{
	GLenum format, type;
	pixel_format(internal, &format, &type);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindTexture(GL_TEXTURE_2D, t);
	/* An unpack buffer bound would be read as the pixels. */
	gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	for (GLsizei l = 0; l < levels; l++) {
		const GLsizei lw = w >> l > 0 ? w >> l : 1;
		const GLsizei lh = h >> l > 0 ? h >> l : 1;
		gl.TexImage2D(GL_TEXTURE_2D, l, (GLint)internal, lw, lh, 0, format,
					  type, NULL);
	}
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
}

static void APIENTRY texture_sub_image(GLuint t, GLint level, GLint x,
									   GLint y, GLsizei w, GLsizei h,
									   GLenum format, GLenum type,
									   const void *px)
{
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindTexture(GL_TEXTURE_2D, t);
	gl.TexSubImage2D(GL_TEXTURE_2D, level, x, y, w, h, format, type, px);
}

static void APIENTRY bind_texture_unit(GLuint unit, GLuint t)
{
	gl.ActiveTexture(GL_TEXTURE0 + unit);
	gl.BindTexture(GL_TEXTURE_2D, t);
}

/* A sampler is made by its name's generation, so its parameters can be set
 * before it is first bound. */
static void APIENTRY create_samplers(GLsizei n, GLuint *s)
{
	gl.GenSamplers(n, s);
}

/* ---- renderbuffers and framebuffers -------------------------------------- */

static void APIENTRY create_renderbuffers(GLsizei n, GLuint *r)
{
	gl.GenRenderbuffers(n, r);
	for (GLsizei i = 0; i < n; i++)
		gl.BindRenderbuffer(GL_RENDERBUFFER, r[i]);
}

static void APIENTRY renderbuffer_storage(GLuint r, GLenum format, GLsizei w,
										  GLsizei h)
{
	gl.BindRenderbuffer(GL_RENDERBUFFER, r);
	gl.RenderbufferStorage(GL_RENDERBUFFER, format, w, h);
}

static void APIENTRY renderbuffer_storage_ms(GLuint r, GLsizei samples,
											 GLenum format, GLsizei w,
											 GLsizei h)
{
	gl.BindRenderbuffer(GL_RENDERBUFFER, r);
	gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, format, w,
									  h);
}

static GLuint draw_fbo(void)
{
	GLint f = 0;
	gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &f);
	return (GLuint)f;
}

static void APIENTRY create_framebuffers(GLsizei n, GLuint *f)
{
	const GLuint was = draw_fbo();
	gl.GenFramebuffers(n, f);
	for (GLsizei i = 0; i < n; i++)
		gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, f[i]);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, was);
}

static void APIENTRY framebuffer_texture(GLuint f, GLenum attachment,
										 GLuint t, GLint level)
{
	const GLuint was = draw_fbo();
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, f);
	gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D,
							t, level);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, was);
}

static void APIENTRY framebuffer_renderbuffer(GLuint f, GLenum attachment,
											  GLenum target, GLuint r)
{
	const GLuint was = draw_fbo();
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, f);
	gl.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, target, r);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, was);
}

static GLenum APIENTRY framebuffer_status(GLuint f, GLenum target)
{
	(void)target;
	const GLuint was = draw_fbo();
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, f);
	const GLenum s = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, was);
	return s;
}

static void APIENTRY blit_framebuffer(GLuint from, GLuint to, GLint sx0,
									  GLint sy0, GLint sx1, GLint sy1,
									  GLint dx0, GLint dy0, GLint dx1,
									  GLint dy1, GLbitfield mask,
									  GLenum filter)
{
	GLint read_was = 0;
	gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_was);
	const GLuint was = draw_fbo();
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, from);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
	gl.BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_was);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, was);
}

static void APIENTRY clear_framebuffer(GLuint f, GLenum buffer,
									   GLint drawbuffer, const GLfloat *value)
{
	const GLuint was = draw_fbo();
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, f);
	gl.ClearBufferfv(buffer, drawbuffer, value);
	gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, was);
}

/* Vulkan's depth range is the gl41 translation's own (--fixup-clipspace),
 * so the call that would ask for it does nothing. */
static void APIENTRY clip_control(GLenum origin, GLenum depth)
{
	(void)origin;
	(void)depth;
}

/* ---- the table ----------------------------------------------------------- */

void gl_compat_install(GlFuncs *t)
{
	memset(vaos, 0, sizeof vaos);
	real_delete_vertex_arrays = t->DeleteVertexArrays;
	t->DeleteVertexArrays                  = delete_vertex_arrays;
	t->ClipControl                         = clip_control;
	t->CreateBuffers                       = create_buffers;
	t->NamedBufferStorage                  = buffer_storage;
	t->MapNamedBufferRange                 = map_buffer_range;
	t->UnmapNamedBuffer                    = unmap_buffer;
	t->NamedBufferSubData                  = buffer_sub_data;
	t->CreateVertexArrays                  = create_vertex_arrays;
	t->VertexArrayVertexBuffer             = vertex_buffer;
	t->VertexArrayAttribFormat             = attrib_format;
	t->VertexArrayAttribBinding            = attrib_binding;
	t->EnableVertexArrayAttrib             = enable_attrib;
	t->VertexArrayBindingDivisor           = binding_divisor;
	t->CreateTextures                      = create_textures;
	t->TextureStorage2D                    = texture_storage;
	t->TextureSubImage2D                   = texture_sub_image;
	t->BindTextureUnit                     = bind_texture_unit;
	t->CreateSamplers                      = create_samplers;
	t->CreateRenderbuffers                 = create_renderbuffers;
	t->NamedRenderbufferStorage            = renderbuffer_storage;
	t->NamedRenderbufferStorageMultisample = renderbuffer_storage_ms;
	t->CreateFramebuffers                  = create_framebuffers;
	t->NamedFramebufferTexture             = framebuffer_texture;
	t->NamedFramebufferRenderbuffer        = framebuffer_renderbuffer;
	t->CheckNamedFramebufferStatus         = framebuffer_status;
	t->BlitNamedFramebuffer                = blit_framebuffer;
	t->ClearNamedFramebufferfv             = clear_framebuffer;
	t->DrawArraysInstancedBaseInstance     = draw_base_instance;
}

void gl_arena_upload(unsigned buf, const void *map, size_t offset,
					 size_t bytes)
{
	if (bytes)
		gl.NamedBufferSubData(buf, (GLintptr)offset, (GLsizeiptr)bytes,
							  (const char *)map + offset);
}
