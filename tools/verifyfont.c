/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * verifyfont -- check everything mkfont baked, against FreeType itself.
 *
 *   verifyfont <mono.ttf> <sans.ttf> <build-dir>
 *
 * Runs on the build host, against the generated font_data.h, font_atlas.bin
 * and font_cells.bin. Every number is recomputed from the faces and every
 * texel compared with a fresh rasterisation. The rasteriser is shared with
 * mkfont (tools/font_raster.h); nothing after it is.
 *
 *   1. the blob is the size the header's atlases account for, and each
 *      decodes to exactly the extent the header claims
 *   2. every cell a size is baked for is byte-identical to a fresh
 *      rasterisation, at the pen offsets FreeType gives. A cell that cannot
 *      be rasterised is a failure, and every cell in the table is accounted
 *      for: compared, blank, absent by design, or failed
 *   3. no two cells share atlas area unless they are the same patch
 *   4. the range table maps every code point face 0 carries to its own index
 *      and rejects the rest; the ASCII shortcut agrees with it
 *   5. every ASCII glyph fits the monospace cell the pen steps by. Above
 *      ASCII the face's marks and precomposed forms are wider than any cell,
 *      which is noted and not failed. Monospaced sizes only
 *   6. the metrics in font_metrics.h are what the faces and cell sizes yield
 *   7. a rendered capital is the cap height asked for: the one check that
 *      measures rather than recomputes, so the only one that can catch an
 *      OS/2 cap height both tools would read wrongly alike
 *   8. every cell's advance is the face's own
 *   9. a cell is absent exactly where its size's code point set says
 *  10. U+FFFD has ink at every size, so a substitution is visible
 *  11. a size that borrows another's cells (FONT_SIZE_FALLBACK) borrows from
 *      one with the same cell size, cap height and line, which itself
 *      borrows from no one
 */
#include "font_raster.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* app.h's, duplicated so this tool does not need app.h's headers. The
 * asserts pin the size and the order. */
typedef struct {
	int16_t x, y;            /* cell origin: x in TEXELS, y in rows          */
	int16_t w, h;            /* cell size:   w in PIXELS, h in rows          */
	int16_t left, top;       /* from the pen on the baseline, y down, pixels */
	int16_t adv;             /* pen step to the next glyph, PIXELS           */
} GlyphCell;

#include "font_data.h"
#include "font_charset.h"
#include "font_codec.h"

_Static_assert(sizeof(GlyphCell) == 14, "GlyphCell layout changed in app.h");
_Static_assert(offsetof(GlyphCell, x)    == 0  &&
			   offsetof(GlyphCell, y)    == 2  &&
			   offsetof(GlyphCell, w)    == 4  &&
			   offsetof(GlyphCell, h)    == 6  &&
			   offsetof(GlyphCell, left) == 8  &&
			   offsetof(GlyphCell, top)  == 10 &&
			   offsetof(GlyphCell, adv)  == 12,
			   "this copy of GlyphCell has drifted from app.h's");

/* Pixels of skirt FT_LCD_FILTER_DEFAULT adds to each side of a bitmap. */
#define LCD_PAD 1

/* A glyph index no code point reaches. */
#define NO_CP 0xffffffffu

static int fails;
static void fail(const char *msg)
{
	if (++fails < 25) fprintf(stderr, "FAIL: %s\n", msg);
}

/* Exits rather than returning NULL: an allocation failure read as a
 * verification failure would be a wrong answer about a correct bake. */
static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) { fprintf(stderr, "verifyfont: out of memory\n"); exit(2); }
	return p;
}

/* The same, zeroed: -fanalyzer cannot see that font_decode() reads back only
 * bytes it has written. */
static void *xcalloc(size_t n)
{
	void *p = calloc(n ? n : 1, 1);
	if (!p) { fprintf(stderr, "verifyfont: out of memory\n"); exit(2); }
	return p;
}

/* The glyph index the range table gives `cp`, or -1, as glyph_index() does. */
static int range_index(uint32_t cp)
{
	int lo = 0, hi = FONT_RANGE_COUNT - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (cp < FONT_RANGE[mid].first)
			hi = mid - 1;
		else if (cp > FONT_RANGE[mid].last)
			lo = mid + 1;
		else
			return (int)(FONT_RANGE[mid].base +
						 (cp - FONT_RANGE[mid].first));
	}
	return -1;
}

int main(int argc, char **argv)
{
	if (argc != 2 + FONT_FACE_COUNT) {
		fprintf(stderr, "usage: %s <mono.ttf> <sans.ttf> <build-dir>\n",
				argv[0]);
		return 2;
	}
	const char *dir = argv[1 + FONT_FACE_COUNT];
	char path[4096];
	snprintf(path, sizeof path, "%s/font_atlas.bin", dir);
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	fseek(f, 0, SEEK_END);
	long blob_len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (blob_len <= 0) { fprintf(stderr, "%s is empty\n", path); return 2; }
	unsigned char *blob = xmalloc((size_t)blob_len);
	if (fread(blob, 1, (size_t)blob_len, f) != (size_t)blob_len) {
		fprintf(stderr, "cannot read %s\n", path);
		return 2;
	}
	fclose(f);

	/* From the file the .S stub will incbin; nothing links the stub here. */
	snprintf(path, sizeof path, "%s/font_cells.bin", dir);
	FILE *cf = fopen(path, "rb");
	if (!cf) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	fseek(cf, 0, SEEK_END); long clen = ftell(cf); fseek(cf, 0, SEEK_SET);
	/* A -1 from ftell would otherwise die in xmalloc as "out of memory". */
	if (clen <= 0) { fprintf(stderr, "%s is empty\n", path); return 2; }
	unsigned char *cbuf = xmalloc((size_t)clen);
	if (fread(cbuf, 1, (size_t)clen, cf) != (size_t)clen) {
		fprintf(stderr, "cannot read %s\n", path);
		return 2;
	}
	fclose(cf);
	if (clen != (long)FONT_CELLS_LEN) {
		fprintf(stderr, "FAIL: font_cells.bin is %ld bytes, the header says "
						"%u\n", clen, FONT_CELLS_LEN);
		return 1;
	}
	GlyphCell *cell_table = xcalloc(FONT_CELLS_RAW);
	if (!font_decode(cbuf, FONT_CELLS_LEN,
					 (unsigned char *)cell_table, FONT_CELLS_RAW)) {
		fprintf(stderr, "FAIL: the cell table does not decode to %u bytes\n",
				FONT_CELLS_RAW);
		return 1;
	}
	printf("ok   the cell table is %ld bytes, decoding to %u (%.0f%%)\n",
		   clen, FONT_CELLS_RAW, 100.0 * clen / FONT_CELLS_RAW);

	/* ---- 1. blob size, and that every atlas decodes ---------------------- */
	long want = 0, raw = 0;
	for (int si = 0; si < FONT_SCALE_COUNT; si++) {
		/* Each atlas where the one before ends: a header from another bake
		 * beside this blob would otherwise decode past its end. */
		if ((long)FONT_ATLAS_OFF[si] != want) {
			fprintf(stderr, "FAIL: the atlas for scale %d starts at %ld, "
							"the one before ends at %ld\n", FONT_SCALE[si],
					(long)FONT_ATLAS_OFF[si], want);
			return 1;
		}
		want += FONT_ATLAS_LEN[si];
		raw  += (long)FONT_ATLAS_W[si] * FONT_ATLAS_H[si];
	}
	if (want != blob_len) {
		fprintf(stderr, "FAIL: blob is %ld bytes, header accounts for %ld\n",
				blob_len, want);
		return 1;
	}

	unsigned char **atlases = xmalloc(FONT_SCALE_COUNT * sizeof *atlases);
	for (int si = 0; si < FONT_SCALE_COUNT; si++) {
		size_t bytes = (size_t)FONT_ATLAS_W[si] * FONT_ATLAS_H[si];
		atlases[si] = xcalloc(bytes);
		if (!font_decode(blob + FONT_ATLAS_OFF[si], FONT_ATLAS_LEN[si],
						 atlases[si], bytes)) {
			fprintf(stderr, "FAIL: the atlas for scale %d does not decode to "
							"%ux%u\n", FONT_SCALE[si], FONT_ATLAS_W[si],
					FONT_ATLAS_H[si]);
			return 1;
		}
	}
	printf("ok   blob is %ld bytes, %d atlases decoding to %ld (%.0f%%)\n",
		   blob_len, FONT_SCALE_COUNT, raw, 100.0 * blob_len / raw);

	FT_Library ft;
	FT_Face faces[FONT_FACE_COUNT], face;
	if (FT_Init_FreeType(&ft)) return 2;
	for (int i = 0; i < FONT_FACE_COUNT; i++)
		if (font_raster_face(ft, argv[1 + i], &faces[i])) {
			fprintf(stderr, "cannot open %s\n", argv[1 + i]); return 2;
		}
	/* Face 0 carries the code point index and the layout metrics. */
	face = faces[0];
	if (font_raster_filter(ft)) {
		fprintf(stderr, "this FreeType has no LCD filter support\n"); return 2;
	}

	/* Glyph index -> code point: the range table read backwards. */
	uint32_t *cp_of = xmalloc((size_t)FONT_GLYPH_COUNT * sizeof *cp_of);
	for (int i = 0; i < FONT_GLYPH_COUNT; i++)
		cp_of[i] = NO_CP;
	for (int r = 0; r < FONT_RANGE_COUNT; r++)
		for (unsigned u = FONT_RANGE[r].first; u <= FONT_RANGE[r].last; u++) {
			unsigned at = FONT_RANGE[r].base + (u - FONT_RANGE[r].first);
			if (at < FONT_GLYPH_COUNT && cp_of[at] == NO_CP)
				cp_of[at] = u;
		}

	/* ---- 4. range table -------------------------------------------------- */
	/* The whole of Unicode: the count is checked against FONT_GLYPH_COUNT, and
	 * a scan that stopped short would make a complete table look short. */
	int mapped = 0, absent = 0;
	const int fails_before_ranges = fails;
	for (uint32_t cp = 0; cp <= 0x10ffff; cp++) {
		int want_i = range_index(cp);
		int has = FT_Get_Char_Index(face, cp) != 0;
		if (has) {
			if (want_i < 0)
				fail("face carries a code point the table rejects");
			else
				mapped++;
		} else if (want_i >= 0) {
			absent++;   /* indexed, though the face lacks it */
		}
	}
	/* Otherwise the table and the cells disagree about what an index means,
	 * which draws the wrong glyph rather than none. */
	if (mapped + absent != FONT_GLYPH_COUNT) {
		fprintf(stderr, "FAIL: the table indexes %d code points, "
						"FONT_GLYPH_COUNT is %d\n",
				mapped + absent, FONT_GLYPH_COUNT);
		fails++;
	}
	if (fails == fails_before_ranges)
		printf("ok   %d code points map to an index "
			   "(%d table entries the face lacks)\n", mapped, absent);

	/* ---- 4, continued: the ASCII shortcut -------------------------------- */
	/* glyph_index() reads FONT_ASCII[cp] instead of searching, below 128. */
	int ascii_bad = 0;
	for (uint32_t cp = 0; cp < 128; cp++) {
		int want_i = range_index(cp);
		if (FONT_ASCII[cp] != want_i) {
			if (ascii_bad++ < 5)
				fprintf(stderr, "FAIL: FONT_ASCII[0x%02x] is %d, the range "
								"table says %d\n", cp, FONT_ASCII[cp], want_i);
		}
	}
	printf("%s the ASCII shortcut matches the range table\n",
		   ascii_bad ? "FAIL" : "ok  ");
	if (ascii_bad) fails++;

	double cap = font_raster_cap(face);

	/* ---- 6. the derived metrics ------------------------------------------ */
	/* app.h inlines these, so a wrong one silently moves the whole decoration
	 * layout. LINE_CELLS is mkfont's, with no independent source: what is
	 * checked is that every size holds the same ratio to its cell. */
	int metric_bad = 0;
	/* FONT_ADVANCE, which every FONT_ADV_PT is derived from, off the face
	 * as mkfont reads it: 'M', unscaled, in ems. Within a float's rounding,
	 * which is how the header writes it. */
	{
		const double upem = face->units_per_EM
						  ? (double)face->units_per_EM : 1000.0;
		if (FT_Load_Char(face, 'M', FT_LOAD_NO_SCALE) != 0) {
			fprintf(stderr, "FAIL: face 0 has no 'M' to take the advance "
							"from\n");
			metric_bad++;
		} else {
			const double adv_em = face->glyph->metrics.horiAdvance / upem;
			if (fabs(adv_em - (double)FONT_ADVANCE) > 1e-6) {
				fprintf(stderr, "FAIL: FONT_ADVANCE is %.9f, face 0's 'M' "
								"gives %.9f\n", (double)FONT_ADVANCE, adv_em);
				metric_bad++;
			}
		}
	}
	for (int sz = 0; sz < FONT_SIZE_COUNT; sz++) {
		double em = (double)FONT_CAP_CELLS * FONT_CELL_PT[sz] / cap;
		/* A proportional size has no per-size advance and publishes 0. */
		long want_adv = FONT_SIZE_MONO[sz]
					  ? lround((double)FONT_ADVANCE * em) : 0;
		int  want_cap = (int)((double)FONT_CAP_CELLS * FONT_CELL_PT[sz] + 0.5);
		if (FONT_ADV_PT[sz] != want_adv) {
			fprintf(stderr, "FAIL: FONT_ADV_PT[%d] is %d, the face gives %ld\n",
					sz, FONT_ADV_PT[sz], want_adv);
			metric_bad++;
		}
		if (FONT_CAP_PT[sz] != want_cap) {
			fprintf(stderr, "FAIL: FONT_CAP_PT[%d] is %d, %d cells of %d "
							"points give %d\n", sz, FONT_CAP_PT[sz],
					(int)FONT_CAP_CELLS, FONT_CELL_PT[sz], want_cap);
			metric_bad++;
		}
		if (FONT_LINE_PT[sz] % FONT_CELL_PT[sz] ||
			FONT_LINE_PT[sz] / FONT_CELL_PT[sz] !=
			FONT_LINE_PT[0] / FONT_CELL_PT[0]) {
			fprintf(stderr, "FAIL: FONT_LINE_PT[%d] is not a whole number of "
							"cells, or not the same number as size 0\n", sz);
			metric_bad++;
		}
		/* A line that does not clear the capitals on it would overlap. */
		if (FONT_LINE_PT[sz] <= FONT_CAP_PT[sz]) {
			fprintf(stderr, "FAIL: FONT_LINE_PT[%d] (%d) does not clear the "
							"cap height (%d)\n", sz, FONT_LINE_PT[sz],
					FONT_CAP_PT[sz]);
			metric_bad++;
		}
	}
	printf("%s the layout metrics (%d sizes) are what the face yields\n",
		   metric_bad ? "FAIL" : "ok  ", FONT_SIZE_COUNT);
	if (metric_bad) fails++;

	/* ---- 11. the fallback sizes ------------------------------------------ */
	/* cell_for() draws the lender's cell as it stands on the borrower's
	 * baseline. The cells themselves are checked below, as the lender's. */
	int  fb_bad = 0;
	long fb_lent = 0, fb_neither = 0;
	for (int sz = 0; sz < FONT_SIZE_COUNT; sz++) {
		const int fb = FONT_SIZE_FALLBACK[sz];
		if (fb < 0)
			continue;
		if (fb >= FONT_SIZE_COUNT || fb == sz || FONT_SIZE_FALLBACK[fb] >= 0) {
			fprintf(stderr, "FAIL: size %d falls back to %d, which is not "
							"another size with no fallback of its own\n",
					sz, fb);
			fb_bad++;
			continue;
		}
		if (FONT_CELL_PT[fb] != FONT_CELL_PT[sz] ||
			FONT_CAP_PT[fb]  != FONT_CAP_PT[sz]  ||
			FONT_LINE_PT[fb] != FONT_LINE_PT[sz]) {
			fprintf(stderr, "FAIL: size %d falls back to %d, which has a "
							"different cell, cap height or line\n", sz, fb);
			fb_bad++;
		}
		for (int si = 0; si < FONT_SCALE_COUNT; si++) {
			const GlyphCell *own  = cell_table +
				((size_t)si * FONT_SIZE_COUNT + (size_t)sz) * FONT_GLYPH_COUNT;
			const GlyphCell *lent = cell_table +
				((size_t)si * FONT_SIZE_COUNT + (size_t)fb) * FONT_GLYPH_COUNT;
			for (int gi = 0; gi < FONT_GLYPH_COUNT; gi++) {
				if (own[gi].x != FONT_CELL_ABSENT)
					continue;
				if (lent[gi].x != FONT_CELL_ABSENT)
					fb_lent++;
				else
					fb_neither++;
			}
		}
	}
	printf("%s the fallback sizes share a cell, a cap height and a line "
		   "(%ld cells borrowed, %ld in neither)\n",
		   fb_bad ? "FAIL" : "ok  ", fb_lent, fb_neither);
	if (fb_bad) fails++;

	/* ---- 2, 3, 5, 7, 8, 9, 10: every cell -------------------------------- */
	/* Each cell of the table ends in exactly one of the first seven, which
	 * is what lets the total be checked at the end. */
	long checked = 0, blank = 0, absent_ok = 0;
	long mismatch = 0, absent_bad = 0, raster_bad = 0, orphan = 0;
	long adv_bad = 0, cap_bad = 0, cap_seen = 0;
	long notdef_seen = 0, notdef_bad = 0;
	long overflow = 0, worst_over = 0, fitted = 0;
	char worst[128] = "";
	/* Counted apart: only an ASCII overflow is a defect (check 5). */
	long overflow_ascii = 0;
	char worst_ascii[128] = "";

	/* owner[] holds the origin of whichever cell claimed each texel. */
	long overlap = 0;
	for (int si = 0; si < FONT_SCALE_COUNT; si++) {
		size_t aw = FONT_ATLAS_W[si], ah = FONT_ATLAS_H[si];
		const unsigned char *atlas = atlases[si];
		long *owner = xmalloc(aw * ah * sizeof *owner);
		for (size_t i = 0; i < aw * ah; i++) owner[i] = -1;

		for (int sz = 0; sz < FONT_SIZE_COUNT; sz++) {
			FT_Face sf = faces[FONT_SIZE_FACE[sz]];
			double  ppem;
			if (font_raster_size(sf, (double)FONT_CAP_CELLS, FONT_CELL_PT[sz],
								 FONT_SCALE[si], &ppem)) {
				fprintf(stderr, "FAIL: cannot size face %d to %g px for size "
								"%d at scale %d\n", FONT_SIZE_FACE[sz], ppem,
						sz, FONT_SCALE[si]);
				return 1;
			}

			/* The pen step text_draw() uses for a monospaced size: the
			 * table text_advance() reads, which check 6 holds to the face. */
			double adv_pt = FONT_ADV_PT[sz];
			double adv_px = adv_pt * FONT_SCALE[si] / 120.0;
			int    fixed_step = FONT_SIZE_MONO[sz];

			for (int gi = 0; gi < FONT_GLYPH_COUNT; gi++) {
				uint32_t cp = cp_of[gi];
				if (cp == NO_CP) {
					if (orphan++ < 5)
						fprintf(stderr, "FAIL: glyph index %d is reached by no "
								"code point in the range table\n", gi);
					continue;
				}

				const void *cv = &cell_table[((size_t)si * FONT_SIZE_COUNT + sz)
											 * FONT_GLYPH_COUNT + gi];
				GlyphCell c;
				memcpy(&c, cv, sizeof c);

				/* 10. Being in the set does not give U+FFFD ink: Ubuntu Sans
				 * has none, so TEXT_UI bakes FreeType's substitute, glyph 0,
				 * which is an inked box in this face and need not be in
				 * another. */
				if (cp == 0xfffd) {
					notdef_seen++;
					if (c.x == FONT_CELL_ABSENT || !c.w || !c.h) {
						if (notdef_bad++ < 5)
							fprintf(stderr, "FAIL: scale %d size %d: U+FFFD "
									"has no ink, so a substituted glyph would "
									"draw nothing\n", FONT_SCALE[si], sz);
					}
				}

				/* 9. Against the set itself: a marker taken at its word would
				 * let a glyph that should have been baked disappear. */
				int in_set = charset_has(
					FONT_SIZE_RANGE + 2 * FONT_SIZE_RANGE_AT[sz],
					FONT_SIZE_RANGE_N[sz], cp);
				if ((c.x == FONT_CELL_ABSENT) != !in_set) {
					if (absent_bad++ < 5)
						fprintf(stderr, "FAIL: U+%04X scale %d size %d: %s\n",
								cp, FONT_SCALE[si], sz,
								in_set ? "absent but in the size's set"
									   : "baked but not in the size's set");
					continue;
				}
				/* Absent by design. If the size has a fallback, the cell it
				 * draws with is the lender's and is checked as that. */
				if (!in_set) {
					absent_ok++;
					continue;
				}

				/* A present cell that cannot be rasterised cannot be compared,
				 * and one not compared has not passed. */
				int want_adv;
				FT_Error err = font_raster_load(sf, cp, &want_adv);
				if (err) {
					if (raster_bad++ < 5)
						fprintf(stderr, "FAIL: U+%04X scale %d size %d: "
								"FreeType cannot load it (error %d)\n", cp,
								FONT_SCALE[si], sz, (int)err);
					continue;
				}
				/* 8. The only check on what a proportional size steps by. */
				if (c.adv != want_adv) {
					if (adv_bad++ < 5)
						fprintf(stderr, "FAIL: U+%04X scale %d size %d: cell "
								"advance %d, face gives %d\n", cp,
								FONT_SCALE[si], sz, c.adv, want_adv);
				}
				FontRaster g;
				err = font_raster_render(sf, &g);
				if (err) {
					if (raster_bad++ < 5)
						fprintf(stderr, "FAIL: U+%04X scale %d size %d: "
								"FreeType cannot render it (error %d)\n", cp,
								FONT_SCALE[si], sz, (int)err);
					continue;
				}
				const FT_Bitmap *b = g.bitmap;

				/* 7. 'F' has a flat top and sits on the baseline, so its
				 * bitmap is the cap height and nothing else, and it is in
				 * every size's set. One pixel of slack for the grid fitting. */
				if (cp == 'F') {
					double want_cap_px = (double)FONT_CAP_PT[sz] *
										 FONT_SCALE[si] / 120.0;
					cap_seen++;
					if (fabs((double)b->rows - want_cap_px) > 1.0) {
						if (cap_bad++ < 5)
							fprintf(stderr, "FAIL: scale %d size %d: 'F' is %u "
									"rows, %d points of cap height at this "
									"scale is %.1f\n", FONT_SCALE[si], sz,
									b->rows, FONT_CAP_PT[sz], want_cap_px);
					}
				}

				/* Space, NBSP, the soft hyphen: FreeType gives a width and no
				 * rows, mkfont stores 0x0, and text_draw() skips them -- so
				 * their pen offsets are never used and are not compared. */
				if (!b->rows || !b->width) {
					if (c.w || c.h) {
						if (mismatch++ < 5)
							fprintf(stderr, "FAIL: U+%04X is blank but baked "
									"%dx%d\n", cp, c.w, c.h);
					} else blank++;
					continue;
				}

				/* b->width is texels, three to the pixel; c.w is pixels. */
				if ((int)b->width != c.w * 3 || (int)b->rows != c.h) {
					if (mismatch++ < 5)
						fprintf(stderr, "FAIL: U+%04X scale %d size %d: baked "
								"%dx%d, FreeType %ux%u\n", cp, FONT_SCALE[si],
								sz, c.w, c.h, b->width, b->rows);
					continue;
				}

				/* The pen offsets: the texel loop proves the ink and says
				 * nothing about where it is placed. Valid only after the
				 * render. */
				if (c.left != g.left || c.top != g.top) {
					if (mismatch++ < 5)
						fprintf(stderr, "FAIL: U+%04X scale %d size %d: baked "
								"pen offset %d,%d, FreeType gives %d,%d\n",
								cp, FONT_SCALE[si], sz, c.left, c.top,
								g.left, g.top);
					continue;
				}

				/* Bounded before it is dereferenced: the cells and the atlas
				 * are two files, and a stale one beside a fresh one must fail
				 * here rather than read out of bounds. */
				if (c.x < 0 || c.y < 0 || c.x + c.w * 3 > (int)aw ||
					c.y + c.h > (int)ah) {
					if (mismatch++ < 5)
						fprintf(stderr, "FAIL: U+%04X scale %d size %d: cell "
								"at %d,%d+%dx%d lies outside the %zux%zu "
								"atlas\n",
								cp, FONT_SCALE[si], sz, c.x, c.y, c.w * 3, c.h,
								(size_t)aw, (size_t)ah);
					continue;
				}

				for (int y = 0; y < c.h; y++)
					for (int x = 0; x < c.w * 3; x++) {
						size_t o = (size_t)(c.y + y) * aw + (c.x + x);
						/* pitch is signed: negative for a bottom-up bitmap. */
						ptrdiff_t r = (ptrdiff_t)y * b->pitch;
						if (atlas[o] != b->buffer[r + x]) {
							if (mismatch++ < 5)
								fprintf(stderr, "FAIL: U+%04X texel %d,%d: "
										"baked %u, FreeType %u\n", cp, x, y,
										atlas[o], b->buffer[r + x]);
							goto next;
						}
						/* 3. Two cells on one texel are legitimate only as
						 * one patch: mkfont folds identical bitmaps, and
						 * those share an origin. */
						long origin = (long)c.y * (long)aw + (long)c.x;
						if (owner[o] >= 0 && owner[o] != origin &&
							overlap++ < 5)
							fprintf(stderr, "FAIL: U+%04X scale %d size %d "
									"overlaps another cell at texel %zu\n",
									cp, FONT_SCALE[si], sz, o);
						owner[o] = origin;
					}
				checked++;

				/* 5. The filter's one-pixel skirt either side is not ink;
				 * counting it would flag every glyph. */
				double ink_left  = c.left + (double)LCD_PAD;
				double right = c.left + (double)c.w - LCD_PAD;
				if (fixed_step)
					fitted++;
				if (fixed_step && (ink_left < 0.0 || right > adv_px)) {
					overflow++;
					long over = (long)ceil(right - adv_px);
					if (ink_left < 0.0 && (long)-ink_left > over)
						over = (long)-ink_left;
					if (cp < 128) {
						overflow_ascii++;
						if (!worst_ascii[0])
							snprintf(worst_ascii, sizeof worst_ascii,
									 "U+%04X scale %d size %d: ink %.0f..%.0f, "
									 "cell 0..%.0f", cp, FONT_SCALE[si], sz,
									 ink_left, right, adv_px);
					}
					if (over > worst_over) {
						worst_over = over;
						snprintf(worst, sizeof worst,
								 "U+%04X scale %d size %d: ink %.0f..%.0f, "
								 "cell 0..%.0f", cp, FONT_SCALE[si], sz,
								 ink_left, right, adv_px);
					}
				}
			next:;
			}
		}
		free(owner);
	}

	/* Zero seen is not a pass: it means the range table does not map it. */
	if (notdef_seen == 0) {
		fprintf(stderr, "FAIL: U+FFFD is not in the index at all\n");
		notdef_bad++;
	}
	printf("%s U+FFFD has ink at every size, so a substitution is visible "
		   "(%ld checked)\n", notdef_bad ? "FAIL" : "ok  ", notdef_seen);
	if (notdef_bad) fails++;

	printf("%s a capital renders the cap height it was asked for (%ld "
		   "checked)\n", cap_bad ? "FAIL" : "ok  ", cap_seen);
	if (cap_bad) fails++;

	printf("%s every cell's advance is the face's own\n",
		   adv_bad ? "FAIL" : "ok  ");
	if (adv_bad) fails++;

	printf("%s every cell absent exactly where the size's set says (%ld "
		   "absent)\n", absent_bad ? "FAIL" : "ok  ", absent_ok);
	if (absent_bad) fails++;

	printf("%s %ld glyph bitmaps byte-identical to FreeType (%ld blank, %ld "
		   "mismatches)\n", mismatch ? "FAIL" : "ok  ", checked, blank,
		   mismatch);
	if (mismatch) fails++;

	printf("%s every present glyph could be rasterised (%ld could not)\n",
		   raster_bad ? "FAIL" : "ok  ", raster_bad);
	if (raster_bad) fails++;

	/* The count that stops a loop which skipped everything from passing. */
	const long cells = (long)FONT_SCALE_COUNT * FONT_SIZE_COUNT *
					   FONT_GLYPH_COUNT;
	const long failed = mismatch + absent_bad + raster_bad + orphan;
	const int  uncounted = checked + blank + absent_ok + failed != cells;
	printf("%s %ld cells: %ld compared, %ld blank, %ld absent by design, "
		   "%ld failed\n",
		   (uncounted || !checked || orphan) ? "FAIL" : "ok  ",
		   cells, checked, blank, absent_ok, failed);
	if (uncounted)
		fprintf(stderr, "FAIL: those do not add up to the table\n");
	if (!checked)
		fprintf(stderr, "FAIL: no cell was compared at all\n");
	if (uncounted || !checked || orphan) fails++;

	printf("%s no two cells share atlas texels (%ld overlaps)\n",
		   overlap ? "FAIL" : "ok  ", overlap);
	if (overlap) fails++;

	if (overflow_ascii) {
		printf("FAIL %ld ASCII cells overflow the monospace advance\n",
			   overflow_ascii);
		printf("       %s\n", worst_ascii);
		fails++;
	} else {
		printf("ok   every ASCII glyph fits the pen step, over %ld monospaced "
			   "cells\n", fitted);
	}
	if (overflow - overflow_ascii)
		printf("note %ld of %ld non-ASCII monospaced cells are wider than the "
			   "pen step (marks and precomposed forms; worst by %ld px)\n"
			   "       %s\n",
			   overflow - overflow_ascii, fitted, worst_over, worst);

	printf("\n%s\n", fails ? "FAILURES ABOVE" : "all checks passed");
	return fails ? 1 : 0;
}
