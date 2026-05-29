/*
 * fyai_prof.c - lightweight, opt-in phase profiling.
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_prof.h"

#define FYAI_PROF_MAX	32

struct fyai_prof_bucket {
	const char	*label;		/* interned string constant, not owned */
	long long	count;
	long long	sum_us;
	long long	min_us;
	long long	max_us;
};

static bool			prof_enabled;
static struct timespec		prof_base;
static struct fyai_prof_bucket	prof_tab[FYAI_PROF_MAX];
static int			prof_n;

void fyai_prof_init(void)
{
	const char *e = getenv("FYAI_PROFILE");

	prof_enabled = e && *e;
	if (prof_enabled)
		clock_gettime(CLOCK_MONOTONIC, &prof_base);
}

bool fyai_prof_enabled(void)
{
	return prof_enabled;
}

void fyai_prof_stamp(struct timespec *ts)
{
	if (prof_enabled)
		clock_gettime(CLOCK_MONOTONIC, ts);
}

static long long prof_delta_us(const struct timespec *a,
			       const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000000LL +
	       (b->tv_nsec - a->tv_nsec) / 1000LL;
}

static struct fyai_prof_bucket *prof_find(const char *label)
{
	int i;

	for (i = 0; i < prof_n; i++) {
		if (!strcmp(prof_tab[i].label, label))
			return &prof_tab[i];
	}
	if (prof_n >= FYAI_PROF_MAX)
		return NULL;
	prof_tab[prof_n].label = label;
	prof_tab[prof_n].min_us = -1;
	return &prof_tab[prof_n++];
}

static void prof_record(const char *label, long long us)
{
	struct fyai_prof_bucket *b = prof_find(label);

	if (!b)
		return;
	b->count++;
	b->sum_us += us;
	if (b->min_us < 0 || us < b->min_us)
		b->min_us = us;
	if (us > b->max_us)
		b->max_us = us;
}

void fyai_prof_since(const char *label, const struct timespec *from)
{
	struct timespec now;

	if (!prof_enabled)
		return;
	clock_gettime(CLOCK_MONOTONIC, &now);
	prof_record(label, prof_delta_us(from, &now));
}

void fyai_prof_once_from_base(const char *label)
{
	struct timespec now;

	if (!prof_enabled)
		return;
	if (prof_find(label)->count)	/* first send wins */
		return;
	clock_gettime(CLOCK_MONOTONIC, &now);
	prof_record(label, prof_delta_us(&prof_base, &now));
}

void fyai_prof_report(void)
{
	struct fyai_prof_bucket *b;
	double avg;
	int i;

	if (!prof_enabled || !prof_n)
		return;

	fprintf(stderr, "\n[fyai-prof] %-22s %6s %10s %10s %10s %10s\n",
		"phase", "n", "total_ms", "avg_ms", "min_ms", "max_ms");
	for (i = 0; i < prof_n; i++) {
		b = &prof_tab[i];
		avg = b->count ? (double)b->sum_us / b->count : 0.0;

		fprintf(stderr,
			"[fyai-prof] %-22s %6lld %10.3f %10.3f %10.3f %10.3f\n",
			b->label, b->count, b->sum_us / 1000.0, avg / 1000.0,
			(b->min_us < 0 ? 0 : b->min_us) / 1000.0,
			b->max_us / 1000.0);
	}
}
