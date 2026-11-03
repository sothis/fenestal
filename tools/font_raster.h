/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The rasteriser mkfont bakes with and verifyfont checks against: FreeType
 * in, one glyph's coverage and pen metrics out.
 *
 * Shared, because a second copy of these choices -- filter, em, load flags,
 * grid alignment, render mode -- is a different renderer once the two drift,
 * and then the check compares nothing. Shared no further: where a cell sits
 * in the atlas, what a size is baked for and what the metrics come to are
 * mkfont's arithmetic, and a checker that shared it would prove nothing.
 */

#ifndef FONT_RASTER_H
#define FONT_RASTER_H

#include <math.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_LCD_FILTER_H
#include FT_TRUETYPE_TABLES_H

static inline FT_Error font_raster_face(FT_Library ft, const char *path,
										FT_Face *out)
{
	return FT_New_Face(ft, path, 0, out);
}

/* The FIR5 filter every LCD render goes through: without it a stem that
 * lands on one emitter glows in that emitter's colour. It widens each bitmap
 * by a pixel either side. */
static inline FT_Error font_raster_filter(FT_Library ft)
{
	return FT_Library_SetLcdFilter(ft, FT_LCD_FILTER_DEFAULT);
}

/* Cap height as a fraction of the em: OS/2's figure where the face carries
 * one, 0.7 where it does not. Per face, since two faces need different ems
 * to reach one cap height. */
static inline double font_raster_cap(FT_Face f)
{
	double upem = f->units_per_EM ? (double)f->units_per_EM : 1000.0;
	TT_OS2 *os2 = FT_Get_Sfnt_Table(f, FT_SFNT_OS2);
	if (os2 && os2->version >= 2 && os2->sCapHeight > 0)
		return os2->sCapHeight / upem;
	return 0.7;
}

/* Size `f` so that a capital is `cap_cells` cells of `cell_pt` points tall at
 * surface scale `scale120`, in 120ths. `*ppem` is the em that takes, in
 * pixels, set whether or not the face accepted it.
 *
 * Char size, not FT_Set_Pixel_Sizes: the em is fractional and that takes
 * whole pixels. 26.6 at 72 dpi makes points and pixels one unit. */
static inline FT_Error font_raster_size(FT_Face f, double cap_cells,
										int cell_pt, int scale120,
										double *ppem)
{
	double em = cap_cells * cell_pt / font_raster_cap(f);
	*ppem = em * scale120 / 120.0;
	return FT_Set_Char_Size(f, 0, (FT_F26Dot6)lround(*ppem * 64.0), 72, 72);
}

/* Load `cp` into the face's slot and grid-align its outline, ready for
 * font_raster_render(). `*adv` is the pen step in pixels, rounded.
 *
 * Not FT_LOAD_RENDER, which fills the outline where it lies: text_draw()
 * places glyphs on whole pixels, so the outline is shifted left by the
 * fraction its left edge sits past one, which puts the left stem on the
 * grid. Not FT_LOAD_FORCE_AUTOHINT either (DESIGN *Three details*). */
static inline FT_Error font_raster_load(FT_Face f, FT_ULong cp, int *adv)
{
	FT_Error err = FT_Load_Char(f, cp, FT_LOAD_DEFAULT);
	if (err)
		return err;

	FT_GlyphSlot gs = f->glyph;
	*adv = (int)((gs->advance.x + 32) >> 6);
	if (gs->format == FT_GLYPH_FORMAT_OUTLINE) {
		FT_BBox cb;
		FT_Outline_Get_CBox(&gs->outline, &cb);
		FT_Pos frac = cb.xMin & 63;          /* 26.6 fixed point */
		if (frac)
			FT_Outline_Translate(&gs->outline, -frac, 0);
	}
	return 0;
}

/* Valid until the face loads another glyph. */
typedef struct {
	const FT_Bitmap *bitmap;    /* coverage: width in texels, three a pixel */
	int              left;      /* from the pen, pixels                     */
	int              top;       /* from the baseline, y down, pixels        */
} FontRaster;

/* Fill the glyph font_raster_load() left in the slot, three samples to the
 * pixel. An empty outline is not a failure: it comes back with zero rows. */
static inline FT_Error font_raster_render(FT_Face f, FontRaster *out)
{
	FT_GlyphSlot gs = f->glyph;
	FT_Error err = FT_Render_Glyph(gs, FT_RENDER_MODE_LCD);
	if (err)
		return err;

	out->bitmap = &gs->bitmap;
	out->left   = (int)gs->bitmap_left;
	out->top    = -(int)gs->bitmap_top;
	return 0;
}

#endif
