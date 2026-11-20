/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Measuring text: UTF-8 decoding, the code point index, the baked glyph table
 * and the width of a string.
 *
 * Every function here is safe from any thread: the cell table is written
 * once, by text_load_cells(), before any thread is started, and everything
 * after that reads it and the const tables in font_data.h. The titlebar's
 * layout depends on it -- it measures the title from the input thread while
 * the render thread is drawing.
 *
 * Uploading an atlas and putting quads on the screen is the renderer's;
 * cell_row() and cell_for() are shared across that line.
 */

#include "core/app.h"

#include "font_data.h"
#include "font_codec.h"

/* What an unmapped code point draws. Looked up through the same range table as
 * anything else, per call -- a binary search over 109 ranges, against a path
 * that only runs for a code point the face does not carry. */
#define TEXT_NOTDEF_CP 0xfffdu /* REPLACEMENT CHARACTER */

/* One UTF-8 sequence. Returns the byte after it and writes the code point.
 * Anything malformed -- a bad lead byte, a truncated or over-long sequence, a
 * surrogate, a value past the last plane -- is not `ok`, writes U+FFFD and
 * consumes exactly one byte, so a corrupt string still terminates.
 *
 * `ok` is separate because a genuine U+FFFD is not an error: drawing shows
 * the replacement character either way, and input drops only the error. */
const char *utf8_decode(const char *s, uint32_t *out, bool *ok)
{
	const unsigned char *p = (const unsigned char *)s;
	unsigned char c        = p[0];

	int extra;
	uint32_t cp;
	uint32_t min;
	*ok  = false;
	*out = TEXT_NOTDEF_CP;
	if (c < 0x80) {
		*out = c;
		*ok  = true;
		return s + 1;
	} else if ((c & 0xe0) == 0xc0) {
		extra = 1;
		cp    = c & 0x1fu;
		min   = 0x80;
	} else if ((c & 0xf0) == 0xe0) {
		extra = 2;
		cp    = c & 0x0fu;
		min   = 0x800;
	} else if ((c & 0xf8) == 0xf0) {
		extra = 3;
		cp    = c & 0x07u;
		min   = 0x10000;
	} else
		return s + 1;

	for (int i = 1; i <= extra; i++) {
		if ((p[i] & 0xc0) != 0x80)
			return s + 1;
		cp = (cp << 6) | (p[i] & 0x3fu);
	}
	if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
		return s + 1;
	*out = cp;
	*ok  = true;
	return s + 1 + extra;
}

/* For drawing and measuring: malformed input is U+FFFD. Everything drawn is
 * somebody else's string -- an output's model name, a GPU name -- from the
 * compositor and the driver rather than from here. */
const char *utf8_next(const char *s, uint32_t *out)
{
	bool ok;
	return utf8_decode(s, out, &ok);
}

/* Code point -> index into the baked glyph tables, or -1: a binary search
 * over the hundred-odd contiguous ranges mkfont emitted. Below 128 mkfont
 * resolved the answer at the bake, since this runs once per glyph while
 * recording and nearly everything drawn is ASCII; verifyfont checks the two
 * agree. */
static int glyph_index(uint32_t cp)
{
	if (cp < 128)
		return FONT_ASCII[cp];

	int lo = 0, hi = FONT_RANGE_COUNT - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (cp < FONT_RANGE[mid].first)
			hi = mid - 1;
		else if (cp > FONT_RANGE[mid].last)
			lo = mid + 1;
		else
			return (int)(FONT_RANGE[mid].base + (cp - FONT_RANGE[mid].first));
	}
	return -1;
}

/* As above, but never fails: a code point the face does not carry draws the
 * replacement character, and a face without even that draws nothing. */
static int glyph_index_or_notdef(uint32_t cp)
{
	int i = glyph_index(cp);
	return i >= 0 ? i : glyph_index(TEXT_NOTDEF_CP);
}

/* Code points in a UTF-8 string, which is what the monospaced width is a
 * multiple of -- bytes would measure a two-byte character as two columns. */
static int text_chars(const char *s)
{
	int n = 0;
	for (uint32_t cp; *s; n++)
		s = utf8_next(s, &cp);
	return n;
}

/* ---- the cell table ------------------------------------------------------
 *
 * Decoded once (DESIGN *The blob*), before the window exists, because the
 * decoration layout measures the title long before there is a device. Written
 * before any thread exists and read-only thereafter, so there is nothing to
 * free and nothing to synchronise. */
static GlyphCell *cell_table;

FnlResult text_load_cells(void)
{
	/* Once per process, not once per fnl_init(): the table is a property of
	 * the *build*. Not a thread guard, and it need not be: fnl_init() is
	 * [main], and this is the one write to the table there is. */
	if (cell_table)
		return FNL_OK;

	cell_table = malloc(FONT_CELLS_RAW);
	if (!cell_table)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory for the %u-byte glyph table",
						FONT_CELLS_RAW);
	if (!font_decode(font_cells_blob, FONT_CELLS_LEN,
					 (unsigned char *)cell_table, FONT_CELLS_RAW)) {
		free(cell_table);
		cell_table = NULL;
		return fnl_fail(FNL_ERR_SYSTEM,
						"the baked glyph table does not decode to %u bytes",
						FONT_CELLS_RAW);
	}

	/* A point-space measurement of the proportional face needs a rung where a
	 * logical point is a pixel. Checked here because the ladder is mkfont's
	 * and this is the first moment the program sees it. */
	if (FONT_SCALE[scale_index(120)] != 120) {
		free(cell_table);
		cell_table = NULL;
		return fnl_fail(FNL_ERR_SYSTEM,
						"the baked scale ladder has no 1.0 rung, so a "
						"point-space text measurement has nothing to measure "
						"against");
	}
	return FNL_OK;
}

/* One row of the table: the cells for this scale and size, indexed by glyph.
 * NULL, reported once per string, if text_load_cells() has not run -- a
 * startup sequence someone has rearranged, not a runtime condition. */
const GlyphCell *cell_row(int si, TextSize sz)
{
	if (!cell_table) {
		fnl_log(FNL_LOG_ERROR, "text measured or drawn before the glyph table "
							   "was loaded");
		return NULL;
	}
	return cell_table +
		   ((size_t)si * FONT_SIZE_COUNT + (size_t)sz) * FONT_GLYPH_COUNT;
}

/* The cell a code point draws with, or NULL if there is nothing to draw at all.
 * `row` must come from cell_row().
 *
 * Three substitutions live here, and all have to happen identically in the
 * pen and in the measure or a string gets laid out to a width it does not
 * have -- which is why they are not written twice. A code point the face does
 * not carry draws U+FFFD. One this size is not baked for (FONT_CELL_ABSENT;
 * see SIZE_RANGES in tools/mkfont.c) draws with the cell of the size's
 * fallback, FONT_SIZE_FALLBACK, when that size has it: the same scale's row,
 * so the same atlas, and the same cap height and baseline, so the cell is
 * used as it stands -- its own advance included. Failing that, U+FFFD.
 *
 * The fallback row is found from `row` itself rather than passed in, so no
 * caller can hand the pen one pair and the measure another. */
const GlyphCell *cell_for(const GlyphCell *row, uint32_t cp)
{
	int gi = glyph_index_or_notdef(cp);
	if (gi < 0)
		return NULL;

	const GlyphCell *cl = &row[gi];
	if (cl->x != FONT_CELL_ABSENT)
		return cl;

	/* The table is [scale][size][glyph], so a row's number is scale *
	 * FONT_SIZE_COUNT + size and its fallback's row is in the same scale. */
	const size_t r  = (size_t)(row - cell_table) / FONT_GLYPH_COUNT;
	const size_t sz = r % FONT_SIZE_COUNT;
	const int fb    = FONT_SIZE_FALLBACK[sz];
	if (fb >= 0) {
		const GlyphCell *alt =
			cell_table + (r - sz + (size_t)fb) * FONT_GLYPH_COUNT;
		if (alt[gi].x != FONT_CELL_ABSENT)
			return &alt[gi];
	}

	int nd = glyph_index(TEXT_NOTDEF_CP);
	return nd < 0 ? NULL : &row[nd];
}

/* The width to reserve for a string, in logical points: no baked scale draws it
 * wider than this.
 *
 * Monospaced is a multiply. Proportional asks every baked scale what it would
 * draw and reserves the widest answer: the pen walks advances rounded to
 * whole pixels, and a sum of rounded advances runs long of the rounded sum,
 * so the 1.0 width alone is too small. Too large is a gap; too small is a
 * collision.
 *
 * Reads nothing but const tables and the decoded cells -- never the scale in
 * force: deco_reserved_rect() runs on the pointer thread as well, and
 * App::draw_scale120 is the render thread's.
 *
 * There is deliberately no measure in *pixels*. If one is ever wanted,
 * convert this width for a monospaced size rather than summing the cells:
 * the pen rounds each point position on its own (see text_draw), which at the
 * 4/3 and 5/3 rungs is not the sum of the cells' advances. */
/* ---- measuring, memoised --------------------------------------------------
 *
 * A UI measures the same strings every frame -- a label's width for the
 * rectangle behind it, a row's for the column it sits in -- and the cost is
 * the whole string re-walked: every baked scale of the face, every character,
 * each a UTF-8 decode and a glyph lookup. The answer depends on the string and
 * the face alone, so it is kept.
 *
 * Keyed on the bytes rather than the pointer, because a caller's buffer is
 * reused: a changed string must miss, and a pointer would not tell. A string
 * too long for the key is never held and always measured, which costs that
 * call what it cost before and bounds the memory. Safe while the cells are
 * loaded once, before any thread exists: a reload would want the table
 * cleared with it. */
#define WIDTH_MEMO 32
#define WIDTH_MEMO_KEY 32

typedef struct {
	TextSize sz;
	unsigned char len; /* the key's bytes; the key is empty at 0 */
	char key[WIDTH_MEMO_KEY];
	int w;
} WidthMemo;

static FNL_THREAD_LOCAL WidthMemo width_memo[WIDTH_MEMO];

static unsigned width_hash(const char *s, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) {
		h ^= (unsigned char)s[i];
		h *= 16777619u;
	}
	return h;
}

static int text_width_uncached(const char *s, TextSize sz)
{
	/* Summed wide and held inside OVERLAY_COORD_MAX: a caller's string
	 * (fnl_text_width()) has no bound on its length, and past the clamp
	 * it is clipped away anyway. */
	if (text_is_mono(sz)) {
		const long long w = (long long)text_chars(s) * text_advance(sz);
		return w > OVERLAY_COORD_MAX ? OVERLAY_COORD_MAX : (int)w;
	}

	long long worst = 0;
	for (int si = 0; si < FONT_SCALE_COUNT; si++) {
		const GlyphCell *row = cell_row(si, sz);
		if (!row)
			return 0; /* nothing decoded yet; cell_for() would fault */
		long long px = 0;
		for (const char *p = s; *p;) {
			uint32_t cp;
			p                   = utf8_next(p, &cp);
			const GlyphCell *cl = cell_for(row, cp);
			if (cl)
				px += cl->adv;
		}
		/* Back to points, rounded up: a point that only half covers what will
		 * be drawn is a point the next thing along may not have. */
		const long long pt = (px * 120 + FONT_SCALE[si] - 1) / FONT_SCALE[si];
		if (pt > worst)
			worst = pt;
	}
	return worst > OVERLAY_COORD_MAX ? OVERLAY_COORD_MAX : (int)worst;
}

int text_width(const char *s, TextSize sz)
{
	const size_t n = strnlen(s, WIDTH_MEMO_KEY);
	WidthMemo *m;
	if (n == WIDTH_MEMO_KEY)
		return text_width_uncached(s, sz);

	/* One slot, hash-tagged and compared in full: a collision is a miss, so a
	 * wrong answer is never returned, and the table is bounded however many
	 * strings a caller measures. */
	m = &width_memo[width_hash(s, n) % WIDTH_MEMO];
	if (m->len == n && m->sz == sz && memcmp(m->key, s, n) == 0)
		return m->w;

	m->w = text_width_uncached(s, sz);
	memcpy(m->key, s, n);
	m->key[n] = '\0';
	m->len    = (unsigned char)n;
	m->sz     = sz;
	return m->w;
}

/* Whether the surface's scale is past either end of the baked ladder. Inside
 * it the nearest atlas is used exactly as baked. Outside there is no near
 * rung -- the 2.0 atlas on a 3.0 surface draws at two thirds the size the
 * layout steps at -- so the quad is scaled to the size the layout wants: soft
 * and the right size. It then no longer lands on whole pixels, and the
 * subpixel path is switched off, since its three samples are the pixel's
 * three emitters only while the atlas and the grid agree (see text_end). */
bool text_resampled(const App *a)
{
	int s120 = scale120(a);
	return s120 > FONT_SCALE[FONT_SCALE_COUNT - 1] || s120 < FONT_SCALE[0];
}

int scale_index(int s120)
{
	int best = 0, best_d = -1;
	for (int i = 0; i < FONT_SCALE_COUNT; i++) {
		int d = FONT_SCALE[i] - s120;
		if (d < 0)
			d = -d;
		if (best_d < 0 || d < best_d) {
			best_d = d;
			best   = i;
		}
	}
	return best;
}

/* See fnl_text_elide(). A cut lands on a code point boundary and is ended in
 * "..." -- a glyph that does not fit cannot be clipped instead, since the
 * frame's glyphs are one draw.
 *
 * Binary search over where to cut, measuring the buffer itself: backing off a
 * character at a time costs a measure per character dropped, which is a title
 * in a narrow column times two. text_width() is monotone in the prefix, every
 * advance being non-negative, so the search is sound. */
void text_elide(char *dst, size_t cap, const char *src, int max_w, TextSize sz)
{
	if (!dst || cap == 0)
		return;
	dst[0] = '\0';
	if (!src || !src[0] || max_w <= 0)
		return;

	/* The dots and the terminator come out of the same buffer, so a cut leaves
	 * room for both; a buffer of four bytes or fewer ends without them. */
	const size_t dots_n = cap > 4 ? 3 : 0;
	const size_t lim    = cap - dots_n - 1;
	size_t n            = 0;
	while (src[n] && src[n] != '\n' && src[n] != '\r' && n < lim) {
		dst[n] = src[n];
		n++;
	}
	/* One line only, so a layout that reserved a line cannot be pushed down by
	 * its content; and a cut on a code point boundary whatever happens next.
	 * The line's own byte decides it: dst[n] is the terminator's place and
	 * holds nothing yet. */
	while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80)
		n--;
	dst[n] = '\0';
	if (text_width(dst, sz) <= max_w)
		return;
	if (dots_n == 0)
		return; /* truncated, and no room to say it was cut */
	if (text_width("...", sz) > max_w) {
		dst[0] = '\0'; /* not even the dots fit */
		return;
	}

	/* The longest prefix whose dots still fit. Each probe measures the line
	 * with the dots written over its end: the reserve width of a concatenation
	 * is not the sum of the two, since each takes the widest of the baked
	 * scales, so it has to be measured whole. The bytes a probe wrote are put
	 * back, so every probe sees the whole line. */
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo + 1) / 2;
		while (mid > 0 && ((unsigned char)dst[mid] & 0xc0) == 0x80)
			mid--;
		if (mid == lo)
			break; /* no whole code point left between */
		char tail[4];
		memcpy(tail, dst + mid, sizeof tail);
		memcpy(dst + mid, "...", 4);
		const bool fits = text_width(dst, sz) <= max_w;
		memcpy(dst + mid, tail, sizeof tail);
		if (fits)
			lo = mid;
		else
			hi = mid - 1;
	}
	dst[lo] = '\0';
	memcpy(dst + lo, "...", dots_n + 1); /* the terminator with them */
}

/* The widest prefix of `s` -- at most `n` bytes, cut on a code point boundary
 * -- that no baked scale draws wider than max_w points. One pass, every scale
 * advanced together, since the reserve width is the widest of them and a
 * prefix's is not the sum of the two.
 *
 * The measures of a *substring* that no terminator ends, which is what a
 * wrapper needs and text_width() cannot do. */
size_t text_prefix_fit(const char *s, size_t n, int max_w, TextSize sz)
{
	if (max_w <= 0)
		return 0;
	if (text_is_mono(sz)) {
		const int adv = text_advance(sz);
		if (adv <= 0)
			return 0;
		size_t i          = 0;
		const size_t most = (size_t)(max_w / adv);
		while (i < n && s[i] && i < most) {
			uint32_t cp;
			const char *q = utf8_next(s + i, &cp);
			if ((size_t)(q - s) > n)
				break;
			i = (size_t)(q - s);
		}
		return i;
	}

	long long px[FONT_SCALE_COUNT] = { 0 };
	size_t i = 0, last = 0;
	while (i < n && s[i]) {
		uint32_t cp;
		const char *q    = utf8_next(s + i, &cp);
		const size_t len = (size_t)(q - s) - i;
		if (i + len > n)
			break;
		long long worst = 0;
		for (int si = 0; si < FONT_SCALE_COUNT; si++) {
			const GlyphCell *row = cell_row(si, sz);
			const GlyphCell *cl  = row ? cell_for(row, cp) : NULL;
			if (cl)
				px[si] += cl->adv;
			const long long pt =
				(px[si] * 120 + FONT_SCALE[si] - 1) / FONT_SCALE[si];
			if (pt > worst)
				worst = pt;
		}
		if (worst > max_w)
			break;
		i += len;
		last = i;
	}
	return last;
}

/* See fnl_text_wrap(). Greedy: the longest prefix that fits, backed up to the
 * last space or break inside it, and a word longer than the line broken where
 * it must be. A run of spaces, '\n's or '\r's separates two lines and is not
 * itself a line, so a trailing one adds none and what comes out has exactly
 * as many lines as this answers.
 *
 * strlen() once a line is the one cost worth naming: what is wrapped is a
 * panel's text, not a file's. */
int text_wrap(char *dst, size_t cap, const char *src, int max_w, TextSize sz)
{
	size_t w  = 0;
	int lines = 0;

	if (!dst || cap == 0)
		return 0;
	dst[0] = '\0';
	if (!src || !src[0] || max_w <= 0)
		return 0;

	const char *p = src;
	while (*p) {
		while (*p == ' ' || *p == '\n' || *p == '\r')
			p++; /* the run between two lines */
		if (!*p)
			break; /* a trailing run is no line */

		const size_t avail = strlen(p);
		size_t fit         = text_prefix_fit(p, avail, max_w, sz);
		if (fit == 0) {
			/* Not one code point fits: take one anyway, so a width too small
			 * to draw anything is a clipped line rather than a loop. */
			uint32_t cp;
			const char *q = utf8_next(p, &cp);
			fit           = (size_t)(q - p);
			if (fit > avail)
				fit = avail;
		}
		size_t take = fit;
		if (fit < avail && p[fit] != ' ' && p[fit] != '\n' && p[fit] != '\r') {
			/* The break lands inside a word: back up to the last space or
			 * break within what fits. */
			size_t at  = 0;
			bool found = false;
			for (size_t i = 0; i < fit; i++)
				if (p[i] == ' ' || p[i] == '\n' || p[i] == '\r') {
					at    = i;
					found = true;
				}
			if (found)
				take = at;
		}
		for (size_t i = 0; i < take; i++)
			if (p[i] == '\n' || p[i] == '\r') {
				take = i; /* never cross a break in the source */
				break;
			}
		/* Room for the break before this line, the line itself and the
		 * terminator after it -- every term of it size_t, since subtracting
		 * first is how a full buffer wrapped round to a huge room and the
		 * line went past the end of it. */
		const size_t fixed = (lines ? 1 : 0) + 1;
		if (w + fixed + take > cap) {
			if (cap < w + fixed)
				break; /* no room for a line at all */
			take = cap - w - fixed;
			if (take == 0)
				break;
		}
		if (lines)
			dst[w++] = '\n';
		memcpy(dst + w, p, take);
		w += take;
		dst[w] = '\0';
		lines++;
		p += take;
	}
	return lines;
}
