/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * mkfont -- bake the overlay's typefaces into subpixel coverage atlases.
 *
 *   mkfont <mono.ttf> <sans.ttf> <out-dir>
 *
 * Writes five files into <out-dir>:
 *
 *   font_atlas.bin   the glyph coverage: one atlas per surface scale, one byte
 *                    per texel and three texels to the pixel, coded
 *                    (see tools/font_codec.h) and concatenated
 *   font_cells.bin   the per-glyph cells, coded the same way
 *   font_atlas.S     an .incbin stub that pulls that file into .rodata
 *   font_data.h      the code point index, the atlas offsets, the sets
 *   font_metrics.h   the numbers the *layout* is measured in, split out so
 *                    that app.h can include them without the tables
 *
 * Runs on the build host; the program links none of this, FreeType included.
 * DESIGN *Text* has the reasoning behind the three rules the output follows:
 * one atlas per scale, because grid-fitting is only valid for the grid it was
 * computed for; a texel is raw coverage, with no transfer function; three
 * texels to the pixel, so cells carry x in texels and w in pixels (GlyphCell
 * in src/core/app.h).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "font_raster.h"

/* The three tables the rest of the program is sized against (DESIGN *Three
 * tables decide the cost*). CELL is one entry per TextSize in src/core/app.h,
 * which asserts that count against the FONT_SIZE_COUNT emitted below. */
static const int   CELL[]   = { 2, 3, 2 };       /* points per cell, per size */
/* Which face each size is cut from, as an index into the TTFs on the command
 * line: Ubuntu Sans Mono, then Ubuntu Sans. */
static const int   FACE[]   = { 0, 0, 1 };
/* Every scale mutter's ladder reaches up to 2.0, in 120ths. */
static const int   SCALES[] = { 120, 150, 160, 180, 200, 240 };
#define SIZE_COUNT  ((int)(sizeof CELL   / sizeof CELL[0]))
#define SCALE_COUNT ((int)(sizeof SCALES / sizeof SCALES[0]))

/* What each size is baked for, as inclusive code point ranges. This is where
 * the megabytes are; DESIGN *Three tables* has the cost of each.
 *
 * TEXT_BODY draws strings nothing here authors -- an output's name, a GPU's
 * -- so it carries every script one of those could be written in.
 * TEXT_LARGE carries what "%d FPS" can contain; a caller drawing anything
 * else at that size must widen it. TEXT_UI is a caller's window title, baked
 * for Latin and punctuation only: anything else TEXT_BODY carries is drawn
 * with TEXT_BODY's cell (FALLBACK).
 *
 * U+FFFD is in every set: cell_for() substitutes it for a code point neither
 * the size nor its fallback carries. */
static const unsigned BODY_RANGES[] = {
	0x0000, 0x017f,     /* ASCII, Latin-1, Latin Ext-A                       */
	0x0250, 0x036f,     /* IPA, spacing modifiers, combining marks           */
	0x0370, 0x052f,     /* Greek, Cyrillic                                   */
	0x1e00, 0x1eff,     /* Latin Ext Additional -- Vietnamese                */
	0x2000, 0x218f,     /* punctuation, sub/superscripts, currency, TM, No   */
	0xfb00, 0xffff,     /* ligatures, and the specials that hold U+FFFD      */
};
static const unsigned LARGE_RANGES[] = {
	' ',    ' ',
	'0',    '9',
	'F',    'F',
	'P',    'P',
	'S',    'S',
	0xfffd, 0xfffd,
};
static const unsigned UI_RANGES[] = {
	0x0000, 0x017f,     /* ASCII, Latin-1, Latin Ext-A                       */
	0x2000, 0x218f,     /* punctuation, sub/superscripts, currency, TM, No   */
	0xfffd, 0xfffd,
};
#define RANGE_PAIRS(a) ((int)(sizeof (a) / sizeof (a)[0] / 2))
static const struct { const unsigned *r; int n; } SIZE_RANGES[SIZE_COUNT] = {
	{ BODY_RANGES,  RANGE_PAIRS(BODY_RANGES)  },   /* TEXT_BODY  */
	{ LARGE_RANGES, RANGE_PAIRS(LARGE_RANGES) },   /* TEXT_LARGE */
	{ UI_RANGES,    RANGE_PAIRS(UI_RANGES)    },   /* TEXT_UI    */
};
/* The size whose cell draws a code point this size is not baked for, or -1.
 * Only a size of the same CELL, so the same cap height and baseline, and
 * never a chain; main() checks both. */
static const int FALLBACK[] = { -1, -1, 0 };
_Static_assert(sizeof FALLBACK / sizeof FALLBACK[0] ==
			   sizeof CELL / sizeof CELL[0],
			   "FALLBACK[] and CELL[] disagree about how many text sizes "
			   "there are");
#define FACE_COUNT 2
_Static_assert(sizeof FACE / sizeof FACE[0] == SIZE_COUNT,
			   "FACE[] and CELL[] disagree about how many text sizes there "
			   "are");

#define CAP_CELLS   5.0f
/* Baseline to baseline, in cells. */
#define LINE_CELLS  9

/* Shelf width, in texels. Nothing needs a power of two. */
#define ATLAS_W 1536

#define MAX_GLYPHS 8192

/* `w` is texels, three per pixel, which is what packs and copies; the header
 * is given `w / 3`, pixels, which is what the quad is sized by. */
typedef struct {
	int            w, h;        /* bitmap size: w in texels, h in rows       */
	int            left, top;   /* from the pen on the baseline, y down, px  */
	int            x, y;        /* where it landed in the atlas, x in texels */
	unsigned char *bm;          /* coverage, w*h, or NULL for a blank glyph  */
	int            dup;         /* index of an identical earlier cell, or -1 */
	int            absent;      /* this size is not baked for this glyph     */
	int            adv;         /* pen step for this glyph, in pixels        */
} Cell;

#include "font_charset.h"
#define FONT_CODEC_ENCODE
#include "font_codec.h"

/* Close a file this tool wrote. A full disk shows up as a sticky error on the
 * stream or at fclose()'s flush, not in what fprintf() returns, which is why
 * nothing here checks fprintf(). errno is fclose()'s only when fclose()
 * failed; a sticky error's may have been overwritten since. */
static int close_out(FILE *f, const char *what)
{
	int bad = ferror(f);
	if (fclose(f) != 0) {
		fprintf(stderr, "mkfont: write failed on %s: %s\n", what,
				strerror(errno));
		return 1;
	}
	if (bad) {
		fprintf(stderr, "mkfont: write failed on %s\n", what);
		return 1;
	}
	return 0;
}

/* Every output is written under a .new name and renamed into place only once
 * all of them are whole, as the shader rule in CMakeLists.txt does: a bake
 * that stopped part-way would otherwise leave a truncated file newer than the
 * fonts, and the next make would not bake again. In rename order, the headers
 * last. */
static const char *const OUTPUTS[] = {
	"font_atlas.bin", "font_cells.bin", "font_atlas.S", "font_metrics.h",
	"font_data.h",
};

static FILE *open_out(const char *dir, const char *name, const char *mode)
{
	char path[4096];
	snprintf(path, sizeof path, "%s/%s.new", dir, name);
	FILE *f = fopen(path, mode);
	if (!f)
		fprintf(stderr, "mkfont: cannot write %s: %s\n", path,
				strerror(errno));
	return f;
}

static int publish(const char *dir)
{
	for (size_t i = 0; i < sizeof OUTPUTS / sizeof OUTPUTS[0]; i++) {
		char from[4096], to[4096];
		snprintf(from, sizeof from, "%s/%s.new", dir, OUTPUTS[i]);
		snprintf(to, sizeof to, "%s/%s", dir, OUTPUTS[i]);
		if (rename(from, to) != 0) {
			fprintf(stderr, "mkfont: cannot rename %s to %s: %s\n", from, to,
					strerror(errno));
			return 1;
		}
	}
	return 0;
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) { fprintf(stderr, "mkfont: out of memory\n"); exit(1); }
	return p;
}

/* The same, zeroed, for the cell array: -fanalyzer cannot see that the fill
 * loop writes every element the dedup pass reads. */
static void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size);
	if (!p) { fprintf(stderr, "mkfont: out of memory\n"); exit(1); }
	return p;
}

int main(int argc, char **argv)
{
	if (argc != 2 + FACE_COUNT) {
		fprintf(stderr, "usage: %s <mono.ttf> <sans.ttf> <out-dir>\n",
				argv[0]);
		return 2;
	}
	const char *dir = argv[1 + FACE_COUNT];

	FT_Library ft;
	FT_Face    faces[FACE_COUNT];
	if (FT_Init_FreeType(&ft)) {
		fprintf(stderr, "mkfont: cannot initialise FreeType\n");
		return 1;
	}
	for (int i = 0; i < FACE_COUNT; i++)
		if (font_raster_face(ft, argv[1 + i], &faces[i])) {
			fprintf(stderr, "mkfont: cannot read %s\n", argv[1 + i]);
			return 1;
		}
	/* Face 0 gives the code point index and the layout metrics. */
	FT_Face face = faces[0];

	for (int i = 0; i < SIZE_COUNT; i++)
		if (FACE[i] < 0 || FACE[i] >= FACE_COUNT) {
			fprintf(stderr, "mkfont: size %d asks for face %d, and only %d "
							"were given\n", i, FACE[i], FACE_COUNT);
			return 1;
		}
	for (int i = 0; i < SIZE_COUNT; i++) {
		const int fb = FALLBACK[i];
		if (fb < 0)
			continue;
		if (fb >= SIZE_COUNT || fb == i || FALLBACK[fb] >= 0 ||
			CELL[fb] != CELL[i]) {
			fprintf(stderr, "mkfont: size %d cannot fall back to size %d: it "
							"must be another size of the same cell, with no "
							"fallback of its own\n", i, fb);
			return 1;
		}
	}

	if (font_raster_filter(ft)) {
		fprintf(stderr, "mkfont: this FreeType has no LCD filter support\n");
		return 1;
	}

	/* ---- the code points the face actually carries ---------------------- */
	/* FreeType walks the cmap in ascending order; the range table and its
	 * binary search rely on that. */
	unsigned *cp = xmalloc(MAX_GLYPHS * sizeof *cp);
	int n = 0;
	FT_UInt  gi;
	FT_ULong c = FT_Get_First_Char(face, &gi);
	while (gi && n < MAX_GLYPHS) {
		cp[n++] = (unsigned)c;
		c = FT_Get_Next_Char(face, c, &gi);
	}
	if (gi) {
		fprintf(stderr, "mkfont: %s maps more than %d code points; raise "
						"MAX_GLYPHS\n", argv[1], MAX_GLYPHS);
		return 1;
	}
	if (!n) {
		fprintf(stderr, "mkfont: %s has no character map\n", argv[1]);
		return 1;
	}

	/* ---- the metrics the layout is written against ---------------------- */
	double upem = face->units_per_EM ? (double)face->units_per_EM : 1000.0;
	double cap  = font_raster_cap(face);

	/* Face 0 is monospaced, so any glyph carries the advance -- and a face
	 * without 'M' is no face to lay text out with: a guessed advance would
	 * bake a pen step every line then disagrees with. */
	if (FT_Load_Char(face, 'M', FT_LOAD_NO_SCALE) != 0) {
		fprintf(stderr, "mkfont: %s has no 'M' to take the advance from\n",
				argv[1]);
		return 1;
	}
	const double adv = face->glyph->metrics.horiAdvance / upem;

	/* ---- rasterise, pack and write one atlas per scale ------------------ */
	FILE *bin = open_out(dir, "font_atlas.bin", "wb");
	if (!bin)
		return 1;

	/* Cells for every (scale, size, glyph), in that order -- the layout the
	 * generated header declares and cell_row() indexes. */
	Cell *cell = xcalloc((size_t)SCALE_COUNT * SIZE_COUNT * (size_t)n,
						 sizeof *cell);
#define CELL_AT(si, sz, i) \
	cell[(((size_t)(si) * SIZE_COUNT + (sz)) * (size_t)n) + (i)]

	unsigned atlas_off[SCALE_COUNT], atlas_w[SCALE_COUNT], atlas_h[SCALE_COUNT];
	unsigned atlas_len[SCALE_COUNT];      /* the encoded length of each */
	unsigned long total = 0;

	for (int si = 0; si < SCALE_COUNT; si++) {
		/* This scale's cells, every size, as the flat run the passes after
		 * the fill walk. */
		Cell *row = &cell[(size_t)si * SIZE_COUNT * (size_t)n];

		/* Every size shares the scale's atlas: a scale is what the program
		 * uploads at a time. */
		for (int sz = 0; sz < SIZE_COUNT; sz++) {
			FT_Face sf = faces[FACE[sz]];
			double  ppem;
			if (font_raster_size(sf, (double)CAP_CELLS, CELL[sz], SCALES[si],
								 &ppem)) {
				fprintf(stderr, "mkfont: cannot set size %g\n", ppem);
				return 1;
			}
			for (int i = 0; i < n; i++) {
				Cell *q = &CELL_AT(si, sz, i);
				memset(q, 0, sizeof *q);
				q->dup = -1;
				/* Not in this size's set: no bitmap, no cell, and a marker in
				 * the header so cell_for() can tell it from a glyph that is
				 * baked and simply blank, like a space. */
				if (!charset_has(SIZE_RANGES[sz].r, SIZE_RANGES[sz].n, cp[i])) {
					q->absent = 1;
					continue;
				}
				/* Fatal, not a `continue`: a skip leaves the cell *present*
				 * with a zero advance, and text_draw() would overprint the
				 * next character rather than substitute U+FFFD. */
				if (font_raster_load(sf, cp[i], &q->adv)) {
					fprintf(stderr, "mkfont: %s cannot load U+%04X at scale %d "
									"size %d, though its own cmap lists it\n",
							argv[1 + FACE[sz]], cp[i], SCALES[si], sz);
					return 1;
				}
				/* Fatal like the load: a skip would bake a blank cell that
				 * steps correctly. */
				FontRaster g;
				if (font_raster_render(sf, &g)) {
					fprintf(stderr, "mkfont: %s cannot render U+%04X at scale "
									"%d size %d\n", argv[1 + FACE[sz]], cp[i],
							SCALES[si], sz);
					return 1;
				}
				q->w    = (int)g.bitmap->width;
				q->h    = (int)g.bitmap->rows;
				q->left = g.left;
				q->top  = g.top;
				if (!q->w || !q->h) { q->w = q->h = 0; continue; }
				q->bm = xmalloc((size_t)q->w * (size_t)q->h);
				for (int y = 0; y < q->h; y++)
					memcpy(q->bm + (size_t)y * q->w,
						   g.bitmap->buffer + (ptrdiff_t)y * g.bitmap->pitch,
						   (size_t)q->w);
			}
		}

		/* Fold byte-identical bitmaps onto one another: marks that grid-fit
		 * alike, and code points a face maps to one glyph. */
		int flat = SIZE_COUNT * n, uniq = 0;
		for (int a = 0; a < flat; a++) {
			Cell *qa = &row[a];
			if (!qa->bm) continue;
			for (int b = 0; b < a; b++) {
				Cell *qb = &row[b];
				if (qb->bm && qb->dup < 0 && qb->w == qa->w && qb->h == qa->h &&
					!memcmp(qb->bm, qa->bm, (size_t)qa->w * (size_t)qa->h)) {
					qa->dup = b;
					break;
				}
			}
			if (qa->dup < 0) uniq++;
		}

		/* Shelf packing: rows of decreasing height, with a gutter so no
		 * neighbour bleeds in. Only the unique cells are placed. */
		int *order = xmalloc((size_t)flat * sizeof *order);
		int  k = 0, maxh = 0, maxw = 0;
		for (int a = 0; a < flat; a++) {
			Cell *q = &row[a];
			if (q->bm && q->dup < 0) {
				if (q->h > maxh) maxh = q->h;
				if (q->w > maxw) maxw = q->w;
			}
		}
		for (int h = maxh; h >= 1; h--)
			for (int a = 0; a < flat; a++) {
				Cell *q = &row[a];
				if (q->bm && q->dup < 0 && q->h == h) order[k++] = a;
			}

		int aw = ATLAS_W;
		/* Room for the widest glyph and its gutter, or each takes a shelf. */
		while (maxw + 3 > aw) aw *= 2;

		int x = 0, y = 0, shelf = 0, used = 0;
		for (int j = 0; j < k; j++) {
			Cell *q = &row[order[j]];
			if (x + q->w + 3 > aw) { x = 0; y += shelf + 1; shelf = 0; }
			q->x = x;
			q->y = y;
			x += q->w + 3;      /* a pixel of padding, in texels */
			if (q->h > shelf) shelf = q->h;
			if (y + q->h > used) used = y + q->h;
		}
		int ah = used ? used : 1;

		/* Duplicates take their twin's patch and keep their own left/top:
		 * the same coverage need not sit at the same offset from the pen. */
		for (int a = 0; a < flat; a++) {
			Cell *q = &row[a];
			if (q->dup >= 0) {
				Cell *t = &row[q->dup];
				q->x = t->x;
				q->y = t->y;
			}
		}

		size_t bytes = (size_t)aw * (size_t)ah;
		unsigned char *px = xmalloc(bytes);
		memset(px, 0, bytes);
		for (int j = 0; j < k; j++) {
			Cell *q = &row[order[j]];
			for (int yy = 0; yy < q->h; yy++)
				for (int xx = 0; xx < q->w; xx++)
					px[(size_t)(q->y + yy) * (size_t)aw +
					   (size_t)(q->x + xx)] = q->bm[(size_t)yy * q->w + xx];
		}
		/* An atlas that grew in coding has stopped looking like coverage. */
		unsigned char *enc = xmalloc(FONT_CODEC_MAX(bytes));
		size_t enc_len = font_encode(px, bytes, enc, FONT_CODEC_MAX(bytes));
		if (!enc_len || enc_len >= bytes) {
			fprintf(stderr, "mkfont: scale %d encodes to %zu bytes from %zu -- "
							"the working set would not allocate, the encode "
							"did not fit its own worst case, or the atlas has "
							"no repetition in it and font_codec.h is the wrong "
							"container for that\n",
					SCALES[si], enc_len, bytes);
			return 1;
		}
		if (fwrite(enc, 1, enc_len, bin) != enc_len) {
			fprintf(stderr, "mkfont: short write on the atlas\n");
			return 1;
		}

		atlas_off[si] = (unsigned)total;
		atlas_len[si] = (unsigned)enc_len;
		atlas_w[si]   = (unsigned)aw;
		atlas_h[si]   = (unsigned)ah;
		total += enc_len;

		fprintf(stderr, "mkfont: scale %d -- %dx%d, %d of %d cells unique, "
						"%.2f MB -> %.2f MB (%.0f%%)\n",
				SCALES[si], aw, ah, uniq, flat, bytes / 1048576.0,
				enc_len / 1048576.0, 100.0 * enc_len / bytes);

		free(enc);
		free(px);
		free(order);
		for (int a = 0; a < flat; a++)
			free(row[a].bm);
	}
	if (close_out(bin, "the atlas"))
		return 1;

	/* ---- the .incbin stub ----------------------------------------------- */
	/* .incbin rather than a C array: megabytes of braced initialiser cost
	 * the compiler seconds and hundreds of megabytes. */
	FILE *s = open_out(dir, "font_atlas.S", "w");
	if (!s)
		return 1;
	fprintf(s, "/* Generated by tools/mkfont -- do not edit. */\n"
			   "    .section .rodata\n"
			   "    .globl font_atlas_blob\n"
			   "    .balign 4\n"
			   "font_atlas_blob:\n"
			   "    .incbin \"%s/font_atlas.bin\"\n"
			   "    .globl font_cells_blob\n"
			   "    .balign 4\n"
			   "font_cells_blob:\n"
			   "    .incbin \"%s/font_cells.bin\"\n"
			   /* Non-executable stack, so the linker does not mark the whole
				* program's stack executable for the sake of one data file. */
			   "    .section .note.GNU-stack,\"\",%%progbits\n", dir, dir);
	if (close_out(s, "the .incbin stub"))
		return 1;

	/* ---- the layout metrics --------------------------------------------- */
	/* A small header of its own: app.h includes it, so every translation
	 * unit folds these in, and none pays for the tables in font_data.h. */
	FILE *m = open_out(dir, "font_metrics.h", "w");
	if (!m)
		return 1;

	fprintf(m, "/* Generated by tools/mkfont -- do not edit.\n"
			   " *\n"
			   " * The overlay's grid, in logical points: how far the pen "
			   "steps\n"
			   " * per glyph, how far one baseline is from the next, and how "
			   "tall\n"
			   " * a capital is. All three are measurements of the typeface, "
			   "not\n"
			   " * numbers chosen here -- see the note on text_advance() in\n"
			   " * tools/mkfont.c. Derived once, at the bake, so the layout "
			   "and\n"
			   " * the atlases cannot come to disagree. */\n\n"
			   "#ifndef FONT_METRICS_H\n#define FONT_METRICS_H\n\n");
	fprintf(m, "#define FONT_SIZE_COUNT  %d\n\n", SIZE_COUNT);

	/* Whole points, or a table's columns drift apart. Zero for a
	 * proportional size, so no caller measures one as though it were a grid. */
	fprintf(m, "/* Pen step per glyph, 0 where the face is proportional. */\n"
			   "static const int FONT_ADV_PT[FONT_SIZE_COUNT]  = {");
	for (int i = 0; i < SIZE_COUNT; i++)
		fprintf(m, "%s%ld", i ? ", " : " ",
				FACE[i] ? 0L
						: lround(adv * ((double)CAP_CELLS * CELL[i] / cap)));
	fprintf(m, " };\n");
	fprintf(m, "/* Whether that step means anything: 0 for the sizes whose\n"
			   " * cells carry an advance each. */\n"
			   "static const int FONT_SIZE_MONO[FONT_SIZE_COUNT] = {");
	for (int i = 0; i < SIZE_COUNT; i++)
		fprintf(m, "%s%d", i ? ", " : " ", FACE[i] ? 0 : 1);
	fprintf(m, " };\n");
	fprintf(m, "/* Baseline to baseline. */\n"
			   "static const int FONT_LINE_PT[FONT_SIZE_COUNT] = {");
	for (int i = 0; i < SIZE_COUNT; i++)
		fprintf(m, "%s%d", i ? ", " : " ", LINE_CELLS * CELL[i]);
	fprintf(m, " };\n");
	fprintf(m, "/* Cap height -- what the overlay centres text boxes against. "
			   "*/\n"
			   "static const int FONT_CAP_PT[FONT_SIZE_COUNT]  = {");
	for (int i = 0; i < SIZE_COUNT; i++)
		fprintf(m, "%s%d", i ? ", " : " ", (int)(CAP_CELLS * CELL[i] + 0.5f));
	fprintf(m, " };\n\n#endif\n");
	if (close_out(m, "font_metrics.h"))
		return 1;

	/* ---- the header ------------------------------------------------------ */
	FILE *h = open_out(dir, "font_data.h", "w");
	if (!h)
		return 1;

	fprintf(h, "/* Generated by tools/mkfont -- do not edit.\n"
			   " *\n");
	for (int i = 0; i < FACE_COUNT; i++)
		fprintf(h, " *   face %d  %s\n", i,
				faces[i]->family_name ? faces[i]->family_name : "?");
	fprintf(h, " *\n"
			   " * Rasterised by FreeType at each surface scale and packed\n"
			   " * into subpixel coverage atlases, three texels to the pixel\n"
			   " * across. One atlas per scale holds every size, whichever\n"
			   " * face each is cut from, because one is uploaded at a time.\n"
			   " * The pixels live in font_atlas.bin and reach the program\n"
			   " * through font_atlas.S; this is everything needed to index\n"
			   " * them.\n"
			   " *\n"
			   " * Included by the text and overlay sources and by\n"
			   " * tools/verifyfont. The handful of numbers the *layout*\n"
			   " * needs are in font_metrics.h, which app.h includes\n"
			   " * everywhere. */\n\n"
			   "#ifndef FONT_DATA_H\n#define FONT_DATA_H\n\n"
			   "#include \"font_metrics.h\"\n\n");

	fprintf(h, "/* Which face each size is cut from, indexed as the faces\n"
			   " * were given to mkfont. Emitted from FACE[] there. */\n"
			   "#define FONT_FACE_COUNT %d\n"
			   "static const int FONT_SIZE_FACE[FONT_SIZE_COUNT] = {",
			FACE_COUNT);
	for (int i = 0; i < SIZE_COUNT; i++)
		fprintf(h, "%s%d", i ? ", " : " ", FACE[i]);
	fprintf(h, " };\n\n");

	fprintf(h, "/* The blob, one atlas per scale, one byte a texel,\n"
			   "   three texels to the pixel across. */\n"
			   "extern const unsigned char font_atlas_blob[];\n\n");

	fprintf(h, "#define FONT_GLYPH_COUNT %d\n", n);
	fprintf(h, "#define FONT_SCALE_COUNT %d\n\n", SCALE_COUNT);

	fprintf(h, "/* Fractions of an em, off face 0 -- the monospaced\n"
			   " * one the layout metrics are derived from. */\n");
	fprintf(h, "#define FONT_CAP_HEIGHT %.9ff\n", cap);
	fprintf(h, "#define FONT_ADVANCE    %.9ff\n", adv);
	fprintf(h, "#define FONT_CAP_CELLS  %.1ff\n\n", (double)CAP_CELLS);

	fprintf(h, "/* Points per cell, per text size -- what the em is derived\n"
			   " * from, and what the atlases were baked against. */\n"
			   "static const int FONT_CELL_PT[FONT_SIZE_COUNT] = {");
	for (int i = 0; i < SIZE_COUNT; i++)
		fprintf(h, "%s%d", i ? ", " : " ", CELL[i]);
	fprintf(h, " };\n\n");

	fprintf(h, "/* A cell whose origin is this is not baked at that size --\n"
			   " * see SIZE_RANGES in tools/mkfont.c. cell_for() answers\n"
			   " * with the fallback size's cell, or failing that with\n"
			   " * U+FFFD, which verifyfont checks has ink at every size.\n"
			   " * Distinct from a baked cell that is simply empty, like a\n"
			   " * space. */\n"
			   "#define FONT_CELL_ABSENT (-1)\n\n");

	/* The sets themselves, for verifyfont, flattened with a per-size window:
	 * the shape charset_has() takes. */
	fprintf(h, "/* What each size is baked for: inclusive first/last code\n"
			   " * point pairs, emitted from SIZE_RANGES in tools/mkfont.c.\n"
			   " * FONT_SIZE_RANGE_AT[sz] is the first pair for that size and\n"
			   " * FONT_SIZE_RANGE_N[sz] how many follow. */\n"
			   "static const unsigned FONT_SIZE_RANGE[] = {\n");
	int at[SIZE_COUNT], pairs = 0;
	for (int i = 0; i < SIZE_COUNT; i++) {
		at[i] = pairs;
		for (int j = 0; j < SIZE_RANGES[i].n; j++, pairs++) {
			fprintf(h, "    0x%04x, 0x%04x,", SIZE_RANGES[i].r[2 * j],
					SIZE_RANGES[i].r[2 * j + 1]);
			if (j == 0)
				fprintf(h, "  /* size %d */", i);
			fputc('\n', h);
		}
	}
	fputs("};\n", h);
	fprintf(h, "static const int FONT_SIZE_RANGE_AT[FONT_SIZE_COUNT] = {");
	for (int i = 0; i < SIZE_COUNT; i++)
		fprintf(h, "%s%d", i ? ", " : " ", at[i]);
	fputs(" };\n", h);
	fprintf(h, "static const int FONT_SIZE_RANGE_N[FONT_SIZE_COUNT] = {");
	for (int i = 0; i < SIZE_COUNT; i++)
		fprintf(h, "%s%d", i ? ", " : " ", SIZE_RANGES[i].n);
	fputs(" };\n\n", h);

	fprintf(h, "/* The size whose cell draws a code point this size is not\n"
			   " * baked for, or -1 for none: emitted from FALLBACK in\n"
			   " * tools/mkfont.c. The two share a cell size, so a cap\n"
			   " * height and a baseline. */\n"
			   "static const int FONT_SIZE_FALLBACK[FONT_SIZE_COUNT] = {");
	for (int i = 0; i < SIZE_COUNT; i++)
		fprintf(h, "%s%d", i ? ", " : " ", FALLBACK[i]);
	fputs(" };\n\n", h);

	fprintf(h, "/* The scales an atlas exists for, in 120ths. A surface on\n"
			   " * some other scale takes the nearest of these, and one past\n"
			   " * either end of them gets its quads scaled (see\n"
			   " * text_resampled). */\n"
			   "static const int FONT_SCALE[FONT_SCALE_COUNT] = {");
	for (int i = 0; i < SCALE_COUNT; i++)
		fprintf(h, "%s%d", i ? ", " : " ", SCALES[i]);
	fprintf(h, " };\n\n");

	fprintf(h, "/* Where each atlas starts in the blob and how many bytes it\n"
			   " * takes there. Both are of the *encoded* stream: the blob is\n"
			   " * coded (see tools/font_codec.h), and W*H below is what\n"
			   " * it decodes to. */\n");
	fprintf(h, "static const unsigned FONT_ATLAS_OFF[FONT_SCALE_COUNT] = {");
	for (int i = 0; i < SCALE_COUNT; i++)
		fprintf(h, "%s%uu", i ? ", " : " ", atlas_off[i]);
	fprintf(h, " };\n");
	fprintf(h, "static const unsigned FONT_ATLAS_LEN[FONT_SCALE_COUNT] = {");
	for (int i = 0; i < SCALE_COUNT; i++)
		fprintf(h, "%s%uu", i ? ", " : " ", atlas_len[i]);
	fprintf(h, " };\n");
	fprintf(h, "static const unsigned FONT_ATLAS_W[FONT_SCALE_COUNT] = {");
	for (int i = 0; i < SCALE_COUNT; i++)
		fprintf(h, "%s%uu", i ? ", " : " ", atlas_w[i]);
	fprintf(h, " };\n");
	fprintf(h, "static const unsigned FONT_ATLAS_H[FONT_SCALE_COUNT] = {");
	for (int i = 0; i < SCALE_COUNT; i++)
		fprintf(h, "%s%uu", i ? ", " : " ", atlas_h[i]);
	fprintf(h, " };\n\n");

	/* Code points as ranges: the cmap is mostly contiguous blocks. */
	int nranges = 1;
	for (int i = 1; i < n; i++)
		if (cp[i] != cp[i - 1] + 1) nranges++;

	fprintf(h, "/* Code point -> glyph index. `base` is the index of `first`;\n"
			   " * the range is contiguous from there. Sorted, for the binary\n"
			   " * search in glyph_index(). */\n"
			   "typedef struct { unsigned first, last, base; } FontRange;\n"
			   "#define FONT_RANGE_COUNT %d\n"
			   "static const FontRange FONT_RANGE[FONT_RANGE_COUNT] = {\n",
			nranges);
	{
		int start = 0, col = 0;
		for (int i = 1; i <= n; i++) {
			if (i == n || cp[i] != cp[i - 1] + 1) {
				fprintf(h, "%s{ 0x%x, 0x%x, %d },",
						col == 0 ? "    " : " ", cp[start], cp[i - 1], start);
				if (++col == 4) { fprintf(h, "\n"); col = 0; }
				start = i;
			}
		}
		if (col) fprintf(h, "\n");
	}
	fprintf(h, "};\n\n");

	/* Below 128 the index is resolved here: nearly everything the overlay
	 * draws is ASCII, and the search would rediscover a constant per glyph. */
	fprintf(h, "/* cp -> glyph index for cp < 128, or -1. See glyph_index(). "
			   "*/\n"
			   "static const short FONT_ASCII[128] = {\n");
	for (unsigned c2 = 0; c2 < 128; c2++) {
		int idx = -1;
		for (int i = 0; i < n; i++)
			if (cp[i] == c2) { idx = i; break; }
		fprintf(h, "%s%d,", c2 % 16 == 0 ? "    " : " ", idx);
		if (c2 % 16 == 15) fprintf(h, "\n");
	}
	fprintf(h, "};\n\n");

	/* The cells, coded like the atlas rather than a braced initialiser: most
	 * are the absent marker, which as C source no compressor ever sees. */
	FILE *cf = open_out(dir, "font_cells.bin", "wb");
	if (!cf)
		return 1;

	size_t cell_n = (size_t)SCALE_COUNT * SIZE_COUNT * (size_t)n;
	/* Little-endian int16 per field, in the order GlyphCell declares them,
	 * so the decoded bytes are the struct -- app.h asserts the size and
	 * verifyfont the layout. */
	unsigned char *craw = xmalloc(cell_n * 7 * 2);
	size_t co = 0;
	for (int si = 0; si < SCALE_COUNT; si++)
		for (int sz = 0; sz < SIZE_COUNT; sz++)
			for (int i = 0; i < n; i++) {
				const Cell *q = &CELL_AT(si, sz, i);
				int f[7];
				if (q->absent) {
					f[0] = -1; f[1] = f[2] = f[3] = f[4] = f[5] = f[6] = 0;
				} else {
					/* x in texels, w in pixels. A negative x is an origin
					 * no placed cell can have: FONT_CELL_ABSENT. */
					f[0] = q->x; f[1] = q->y; f[2] = q->w / 3; f[3] = q->h;
					f[4] = q->left; f[5] = q->top; f[6] = q->adv;
				}
				for (int k = 0; k < 7; k++) {
					/* Nothing upstream caps a field to int16 -- q->y is an
					 * atlas row -- and with FNL_VERIFY_FONT off this is the
					 * last check before silent corruption. */
					if (f[k] < -32768 || f[k] > 32767) {
						fprintf(stderr, "mkfont: cell field %d is %d, which "
										"does not fit int16\n", k, f[k]);
						return 1;
					}
					craw[co++] = (unsigned char)(f[k] & 0xff);
					craw[co++] = (unsigned char)((f[k] >> 8) & 0xff);
				}
			}

	unsigned char *cenc = xmalloc(FONT_CODEC_MAX(co));
	size_t cenc_len = font_encode(craw, co, cenc, FONT_CODEC_MAX(co));
	if (!cenc_len || cenc_len >= co) {
		fprintf(stderr, "mkfont: the cell table encodes to %zu bytes from %zu, "
						"which is not a saving\n", cenc_len, co);
		return 1;
	}
	if (fwrite(cenc, 1, cenc_len, cf) != cenc_len) {
		fprintf(stderr, "mkfont: short write on the cell table\n");
		return 1;
	}
	if (close_out(cf, "the cell table"))
		return 1;
	fprintf(stderr, "mkfont: cells -- %zu of them, %.0f KB -> %.0f KB "
					"(%.0f%%)\n",
			cell_n, co / 1024.0, cenc_len / 1024.0, 100.0 * cenc_len / co);
	free(craw); free(cenc);

	fprintf(h, "/* [scale][size][glyph], as a blob: see the note in mkfont on\n"
			   " * why it is not a braced initialiser. FONT_CELLS_RAW is what "
			   "it\n"
			   " * decodes to, FONT_CELLS_LEN what it takes in .rodata. Reach "
			   "it\n"
			   " * decoded once by text_load_cells() and read through\n"
			   " * cell_row() and cell_for(). */\n"
			   "extern const unsigned char font_cells_blob[];\n"
			   "#define FONT_CELLS_LEN %zuu\n"
			   "#define FONT_CELLS_RAW %zuu\n\n", cenc_len, co);
	fprintf(h, "#endif\n");
	if (close_out(h, "font_data.h") || publish(dir))
		return 1;

	fprintf(stderr, "mkfont: %d code points in %d ranges, %d scales, "
					"%.2f MB of atlas\n",
			n, nranges, SCALE_COUNT, total / 1048576.0);

	free(cell);
	free(cp);
	for (int i = 0; i < FACE_COUNT; i++)
		FT_Done_Face(faces[i]);
	FT_Done_FreeType(ft);
	return 0;
}
