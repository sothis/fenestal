/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the benchmark's series of values come to: a distribution of seven
 * figures, and the "low" frame rates. Percentiles are by nearest rank over
 * a sorted copy -- no interpolation, so every figure is a value that was
 * measured.
 */

#include "bench.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void bench_series_add(BenchSeries *s, double v)
{
	if (s->n == s->cap) {
		if (s->cap >= BENCH_SAMPLES_MAX) {
			s->capped = true;
			return;
		}
		const size_t cap = s->cap ? s->cap * 2 : 1024;
		double *p = realloc(s->v, cap * sizeof *p);
		if (!p) {
			s->capped = true;
			return;
		}
		s->v = p;
		s->cap = cap;
	}
	s->v[s->n++] = v;
}

void bench_series_free(BenchSeries *s)
{
	free(s->v);
	memset(s, 0, sizeof *s);
}

static int cmp_double(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

/* The finite values, sorted, in a copy the caller frees; NULL for none. */
static double *sorted(const BenchSeries *s, size_t *n)
{
	*n = 0;
	if (!s->n)
		return NULL;
	double *v = malloc(s->n * sizeof *v);
	if (!v)
		return NULL;
	for (size_t i = 0; i < s->n; i++)
		if (isfinite(s->v[i]))
			v[(*n)++] = s->v[i];
	if (!*n) {
		free(v);
		return NULL;
	}
	qsort(v, *n, sizeof *v, cmp_double);
	return v;
}

/* Nearest rank: the smallest value with at least p of them at or below. */
static double rank(const double *v, size_t n, double p)
{
	size_t k = (size_t)ceil(p * (double)n);
	if (k < 1)
		k = 1;
	return v[k - 1];
}

BenchDist bench_dist(const BenchSeries *s)
{
	BenchDist d = { 0, BENCH_NAN, BENCH_NAN, BENCH_NAN, BENCH_NAN, BENCH_NAN,
					 BENCH_NAN };
	size_t n;
	double *v = sorted(s, &n);
	if (!v)
		return d;
	double sum = 0;
	for (size_t i = 0; i < n; i++)
		sum += v[i];
	d.n    = n;
	d.mean = sum / (double)n;
	d.min  = v[0];
	d.p50  = rank(v, n, 0.50);
	d.p95  = rank(v, n, 0.95);
	d.p99  = rank(v, n, 0.99);
	d.max  = v[n - 1];
	free(v);
	return d;
}

double bench_low_fps(const BenchSeries *intervals_ms, double frac)
{
	size_t n;
	double *v = sorted(intervals_ms, &n);
	if (!v)
		return BENCH_NAN;
	size_t k = (size_t)ceil(frac * (double)n);
	if (k < 1)
		k = 1;
	double sum = 0;
	for (size_t i = n - k; i < n; i++)
		sum += v[i];
	free(v);
	const double mean = sum / (double)k;
	return mean > 0 ? 1000.0 / mean : BENCH_NAN;
}
