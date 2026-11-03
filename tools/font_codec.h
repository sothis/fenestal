/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The container the baked atlases are stored in: LZ77 matches and literals,
 * through an adaptive binary range coder.
 *
 * Written by tools/mkfont and decoded by tools/verifyfont and, at run time,
 * by src/renderer/vulkan/text.c (one atlas) and src/core/text_measure.c (the
 * cell table). One definition for all four: a format with two implementations
 * has two chances to be a different format. Define FONT_CODEC_ENCODE before
 * including to get the encoder; the program only ever decodes.
 *
 * Ours because nothing may join the link line, and small enough to check by
 * eye. DESIGN *The blob* has what a library's codec would have bought.
 *
 * ---- the format ------------------------------------------------------------
 *
 * One range-coded bit stream and nothing else: no header, no length, no end
 * marker. The decoder is told how many bytes to produce and stops there.
 *
 * The range coder is LZMA's: a 32-bit range, 11-bit probabilities starting at
 * one half and moving a 32nd of the way towards each bit seen, a zero first
 * byte, and a five-byte flush. Every bit below is coded against one adaptive
 * probability; there are no direct bits.
 *
 * A sequence of symbols, until the output is full:
 *
 *     is_match       one bit; probability chosen by whether the symbol before
 *                    was a match (it was not, for the first)
 *
 *   a literal, when it is 0:
 *     byte           eight bits, MSB first, through a 256-node bit tree; one
 *                    tree per context, the context being the byte before it
 *                    (0 at the start): 0, 255, or one of four quarters
 *
 *   a match, when it is 1:
 *     is_rep         one bit: the offset is the last match's again (a row
 *                    repeating the one above; a run cut into matches)
 *     offset         unless is_rep. 1..65535, as `nb`, the position of its
 *                    top set bit (0..15), through a 16-node bit tree; then
 *                    the nb bits below that one, MSB first, each against the
 *                    probability for (nb, bit position).
 *     length         length - MIN_MATCH, 0..255, through a 256-node bit tree
 *
 * An offset is measured back from the current output position and may be
 * shorter than the match, which is what makes a run. A match longer than
 * MAX_MATCH is two matches.
 *
 * The stream ends when the output does. A decoder that has then consumed
 * exactly its input, with its code register back at zero -- which the flush
 * guarantees -- is done; anything else was the wrong stream.
 *
 * Decoding costs at most nine coded bits per byte produced (a literal), and
 * at most twenty-nine for the four or more a match produces.
 */

#ifndef FONT_CODEC_H
#define FONT_CODEC_H

#include <stddef.h>
#include <stdint.h>

/* The match finder hashes four bytes, so that is the shortest it finds. */
#define FONT_CODEC_MIN_MATCH 4
/* The length tree has 256 leaves. */
#define FONT_CODEC_MAX_MATCH (FONT_CODEC_MIN_MATCH + 255)
/* The offset's top bit is one of sixteen. */
#define FONT_CODEC_WINDOW    65535

#define FONT_RC_TOP   (1u << 24)   /* renormalise when the range is below */
#define FONT_RC_BITS  11
#define FONT_RC_ONE   (1u << FONT_RC_BITS)
#define FONT_RC_MOVE  5

/* Literal contexts: what the byte before was. */
#define FONT_LIT_CTX  6

/* Where each model sits in the one array, for both sides. */
enum {
	FONT_P_LIT   = 0,                                /* [ctx][256 nodes]      */
	FONT_P_LEN   = FONT_P_LIT + FONT_LIT_CTX * 256,  /* [256 nodes]           */
	FONT_P_SLOT  = FONT_P_LEN + 256,                 /* [16 nodes]            */
	FONT_P_DIST  = FONT_P_SLOT + 16,                 /* [nb][bit position]    */
	FONT_P_MATCH = FONT_P_DIST + 16 * 16,            /* [previous was match]  */
	FONT_P_REP   = FONT_P_MATCH + 2,
	FONT_PROBS   = FONT_P_REP + 1
};

static inline void font_probs_init(uint16_t *p)
{
	for (int k = 0; k < FONT_PROBS; k++)
		p[k] = (uint16_t)(FONT_RC_ONE / 2);
}

static inline unsigned font_lit_ctx(unsigned prev)
{
	return prev == 0 ? 0u : prev == 255 ? 1u : 2u + (prev >> 6);
}

/* ---- decoding ------------------------------------------------------------ */

typedef struct {
	const unsigned char *in;
	size_t   len, pos;
	uint32_t range, code;
	int      bad;           /* a refill wanted a byte the input does not have */
} FontRcDec;

/* One bit against `*p`, which moves towards it. A refill past the end reads
 * as zero and sets `bad`: the caller tests that once a symbol, and bounds
 * every copy itself, so no bit read here can reach outside a buffer. */
static inline unsigned font_rc_bit(FontRcDec *d, uint16_t *p)
{
	if (d->range < FONT_RC_TOP) {
		unsigned b = 0;
		if (d->pos < d->len)
			b = d->in[d->pos++];
		else
			d->bad = 1;
		d->range <<= 8;
		d->code = (d->code << 8) | b;
	}

	const uint32_t bound = (d->range >> FONT_RC_BITS) * *p;
	if (d->code < bound) {
		d->range = bound;
		*p = (uint16_t)(*p + ((FONT_RC_ONE - *p) >> FONT_RC_MOVE));
		return 0;
	}
	d->range -= bound;
	d->code  -= bound;
	*p = (uint16_t)(*p - (*p >> FONT_RC_MOVE));
	return 1;
}

/* `bits` bits, MSB first, through the bit tree at `t`: node 1 is the root and
 * node `s` has children 2s and 2s+1. */
static inline unsigned font_rc_tree(FontRcDec *d, uint16_t *t, unsigned bits)
{
	unsigned s = 1;
	for (unsigned k = 0; k < bits; k++)
		s = (s << 1) | font_rc_bit(d, &t[s]);
	return s - (1u << bits);
}

/* Decode `in_len` bytes into a buffer of exactly `out_len`. Returns 1 only if
 * the stream both filled the buffer and ended with it.
 *
 * No read leaves `in` and no write leaves `out`, whatever the bytes are, and
 * the loop is bounded by `out_len`: every symbol produces at least one byte.
 */
static inline int font_decode(const unsigned char *in, size_t in_len,
							  unsigned char *out, size_t out_len)
{
	uint16_t p[FONT_PROBS];
	size_t   o = 0, last_off = 0;
	unsigned after_match = 0;
	unsigned prev = 0;      /* the byte before `o`: the literal's context */

	if (in_len < 5 || in[0] != 0)
		return 0;
	FontRcDec d = {
		.in = in, .len = in_len, .pos = 5,
		.range = 0xffffffffu,
		.code = ((uint32_t)in[1] << 24) | ((uint32_t)in[2] << 16) |
				((uint32_t)in[3] << 8)  | (uint32_t)in[4],
		.bad = 0,
	};
	font_probs_init(p);

	while (o < out_len) {
		if (!font_rc_bit(&d, &p[FONT_P_MATCH + after_match])) {
			const unsigned ctx = font_lit_ctx(prev);
			prev = font_rc_tree(&d, &p[FONT_P_LIT + ctx * 256], 8);
			out[o++] = (unsigned char)prev;
			after_match = 0;
		} else {
			size_t off = last_off;
			if (!font_rc_bit(&d, &p[FONT_P_REP])) {
				const unsigned nb = font_rc_tree(&d, &p[FONT_P_SLOT], 4);
				off = 1;
				for (unsigned b = nb; b-- > 0; )
					off = (off << 1) |
						  font_rc_bit(&d, &p[FONT_P_DIST + nb * 16 + b]);
			}
			const size_t len = font_rc_tree(&d, &p[FONT_P_LEN], 8) +
							   (size_t)FONT_CODEC_MIN_MATCH;

			/* `off` is zero only for a repeat with no match before it. */
			if (off == 0 || off > o || len > out_len - o)
				return 0;
			/* Byte at a time: the copy may overlap itself, and a wider move
			 * would read ahead of what it has written. */
			for (size_t k = 0; k < len; k++, o++) {
				prev = out[o - off];
				out[o] = (unsigned char)prev;
			}
			last_off = off;
			after_match = 1;
		}
		if (d.bad)
			return 0;
	}

	/* The encoder renormalised after its last bit; this side does it before
	 * the next one, so one byte may still be owed. */
	if (d.range < FONT_RC_TOP) {
		if (d.pos >= d.len)
			return 0;
		d.code = (d.code << 8) | d.in[d.pos++];
	}
	return d.pos == d.len && d.code == 0;
}

#ifdef FONT_CODEC_ENCODE

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* The buffer to hand font_encode() for `n` bytes: more than incompressible
 * input comes to, about nine bits a byte. Not a proven bound, which an
 * adaptive coder does not have -- the encoder checks the capacity it is
 * given and answers 0. */
#define FONT_CODEC_MAX(n) ((n) + (n) / 8 + 64)

#define FONT_CODEC_HASH_BITS  16
#define FONT_CODEC_HASH_SIZE  (1u << FONT_CODEC_HASH_BITS)
/* How far down a hash chain to look. Newest first, so a run finds its
 * offset-1 match on the first candidate. */
#define FONT_CODEC_CHAIN      64
/* A minimum-length match this far back costs more bits to name than the
 * literals it replaces. The encoder's choice; the format does not care. */
#define FONT_CODEC_FAR        4096

typedef struct {
	unsigned char *out;
	size_t         cap, pos;
	uint64_t       low;         /* 32 bits and a carry */
	uint32_t       range;
	unsigned char  cache;       /* the byte held back in case of a carry */
	size_t         cache_size;  /* it, and the 0xff bytes queued behind it */
	int            full;        /* the output did not fit */
} FontRcEnc;

/* Move the top byte of `low` out. It is held back while a carry could still
 * reach it: a byte of 0xff passes a carry on, so those queue behind it. */
static inline void font_rc_shift(FontRcEnc *e)
{
	if ((uint32_t)e->low < 0xff000000u || (e->low >> 32) != 0) {
		const unsigned carry = (unsigned)(e->low >> 32);
		unsigned b = e->cache;
		do {
			if (e->pos < e->cap)
				e->out[e->pos++] = (unsigned char)(b + carry);
			else
				e->full = 1;
			b = 0xff;
		} while (--e->cache_size != 0);
		e->cache = (unsigned char)((uint32_t)e->low >> 24);
	}
	e->cache_size++;
	e->low = (e->low & 0x00ffffffu) << 8;
}

/* font_rc_bit(), from the other side: the same bound, the same update, and
 * the renormalisation that side does before its next bit. */
static inline void font_rc_put(FontRcEnc *e, uint16_t *p, unsigned bit)
{
	const uint32_t bound = (e->range >> FONT_RC_BITS) * *p;
	if (!bit) {
		e->range = bound;
		*p = (uint16_t)(*p + ((FONT_RC_ONE - *p) >> FONT_RC_MOVE));
	} else {
		e->low   += bound;
		e->range -= bound;
		*p = (uint16_t)(*p - (*p >> FONT_RC_MOVE));
	}
	if (e->range < FONT_RC_TOP) {
		e->range <<= 8;
		font_rc_shift(e);
	}
}

/* font_rc_tree(), from the other side. */
static inline void font_rc_put_tree(FontRcEnc *e, uint16_t *t, unsigned bits,
									unsigned v)
{
	unsigned s = 1;
	for (unsigned k = bits; k-- > 0; ) {
		const unsigned bit = (v >> k) & 1u;
		font_rc_put(e, &t[s], bit);
		s = (s << 1) | bit;
	}
}

static inline unsigned font_codec_hash4(const unsigned char *p)
{
	unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8) |
				 ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
	return (v * 2654435761u) >> (32 - FONT_CODEC_HASH_BITS);
}

/* Position `i` joins the chain for the four bytes that start at it. */
static inline void font_codec_insert(int *head, int *prev,
									 const unsigned char *in, size_t i,
									 size_t in_len)
{
	if (i + FONT_CODEC_MIN_MATCH <= in_len) {
		const unsigned hv = font_codec_hash4(in + i);
		prev[i] = head[hv];
		head[hv] = (int)i;
	}
}

/* Encode `in_len` bytes into `out`, which should have room for FONT_CODEC_MAX
 * of them. Returns the length written, or 0 if the working set could not be
 * allocated or the output would not fit. Build-host only.
 *
 * Greedy: the longest match at each position, the last match's offset tried
 * first so that a tie goes to the one that costs a single bit to name.
 */
static inline size_t font_encode(const unsigned char *in, size_t in_len,
								 unsigned char *out, size_t out_cap)
{
	uint16_t p[FONT_PROBS];
	size_t   i = 0, last_off = 0;
	unsigned after_match = 0;

	/* head[] and prev[] hold positions as int, so refuse an input that cannot
	 * be indexed by one; the cast would be silent. */
	if (in_len > (size_t)INT_MAX)
		return 0;

	int *head = malloc(FONT_CODEC_HASH_SIZE * sizeof *head);
	int *prev = malloc((in_len ? in_len : 1) * sizeof *prev);
	if (!head || !prev) { free(head); free(prev); return 0; }
	for (size_t k = 0; k < FONT_CODEC_HASH_SIZE; k++)
		head[k] = -1;

	FontRcEnc e = {
		.out = out, .cap = out_cap, .pos = 0,
		.low = 0, .range = 0xffffffffu,
		.cache = 0, .cache_size = 1, .full = 0,
	};
	font_probs_init(p);

	while (i < in_len) {
		const size_t max_len = in_len - i < FONT_CODEC_MAX_MATCH
								   ? in_len - i : FONT_CODEC_MAX_MATCH;
		size_t best_len = 0, best_off = 0;

		/* last_off was an offset at an earlier position, so it is one here. */
		if (last_off) {
			size_t len = 0;
			while (len < max_len && in[i - last_off + len] == in[i + len])
				len++;
			if (len >= FONT_CODEC_MIN_MATCH) {
				best_len = len;
				best_off = last_off;
			}
		}
		if (i + FONT_CODEC_MIN_MATCH <= in_len) {
			int cand = head[font_codec_hash4(in + i)];
			for (int n = 0; cand >= 0 && n < FONT_CODEC_CHAIN; n++) {
				const size_t off = i - (size_t)cand;
				if (off > FONT_CODEC_WINDOW)
					break;
				size_t len = 0;
				while (len < max_len && in[(size_t)cand + len] == in[i + len])
					len++;
				if (len > best_len) { best_len = len; best_off = off; }
				cand = prev[cand];
			}
		}
		if (best_len == FONT_CODEC_MIN_MATCH && best_off != last_off &&
			best_off > FONT_CODEC_FAR)
			best_len = 0;

		if (best_len < FONT_CODEC_MIN_MATCH) {
			const unsigned ctx = font_lit_ctx(i ? in[i - 1] : 0u);
			font_rc_put(&e, &p[FONT_P_MATCH + after_match], 0);
			font_rc_put_tree(&e, &p[FONT_P_LIT + ctx * 256], 8, in[i]);
			after_match = 0;
			font_codec_insert(head, prev, in, i, in_len);
			i++;
			continue;
		}

		font_rc_put(&e, &p[FONT_P_MATCH + after_match], 1);
		if (best_off == last_off) {
			font_rc_put(&e, &p[FONT_P_REP], 1);
		} else {
			unsigned nb = 0;
			while ((best_off >> (nb + 1)) != 0)
				nb++;
			font_rc_put(&e, &p[FONT_P_REP], 0);
			font_rc_put_tree(&e, &p[FONT_P_SLOT], 4, nb);
			for (unsigned b = nb; b-- > 0; )
				font_rc_put(&e, &p[FONT_P_DIST + nb * 16 + b],
							(unsigned)(best_off >> b) & 1u);
		}
		font_rc_put_tree(&e, &p[FONT_P_LEN], 8,
						 (unsigned)(best_len - FONT_CODEC_MIN_MATCH));
		last_off = best_off;
		after_match = 1;

		/* Or the next match cannot reach back into this one. */
		for (size_t k = 0; k < best_len; k++, i++)
			font_codec_insert(head, prev, in, i, in_len);
	}

	/* Five bytes: the one held back, and the four of `low`. */
	for (int k = 0; k < 5; k++)
		font_rc_shift(&e);

	free(head);
	free(prev);
	return e.full ? 0 : e.pos;
}

#endif /* FONT_CODEC_ENCODE */

#endif
