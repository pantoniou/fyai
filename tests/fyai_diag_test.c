/*
 * fyai_diag_test.c - unit tests for the collected diagnostics
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_CONFIG

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_diag.h"

#define DIAG_THREADS	8
#define DIAG_PER_THREAD	2000

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx;
static int failures;

/* Drain into a temporary file and hand back its contents. */
static char *drain_to_string(struct fyai_diag *diag)
{
	char *buf;
	long size;
	FILE *fp;

	fp = tmpfile();
	if (!fp) {
		fprintf(stderr, "tmpfile() failed\n");
		exit(1);
	}
	diag->fp = fp;
	fyai_diag_drain(diag);
	diag->fp = stderr;

	size = ftell(fp);
	rewind(fp);
	buf = malloc((size_t)size + 1);
	if (!buf || (size && fread(buf, 1, (size_t)size, fp) != (size_t)size)) {
		fprintf(stderr, "failed to read back the drained output\n");
		exit(1);
	}
	buf[size] = '\0';
	fclose(fp);
	return buf;
}

static void expect_str(const char *what, const char *got, const char *want)
{
	if (!strcmp(got, want))
		return;
	fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n", what, got, want);
	failures++;
}

static void expect_eq(const char *what, long long got, long long want)
{
	if (got == want)
		return;
	fprintf(stderr, "FAIL %s: got %lld, want %lld\n", what, got, want);
	failures++;
}

/* An error is bare, matching what the subsystems printed by hand; every lower
 * severity is labelled. */
static void test_format(struct fyai_diag *diag)
{
	char *out;

	fyai_error(&test_ctx, "auth must be auto, api-key, or chatgpt");
	fyai_warning(&test_ctx, "could not persist to config");
	fyai_notice(&test_ctx, "the built-in shell tool is not enabled");

	out = drain_to_string(diag);
	expect_str("severity labelling",
		   out,
		   "config: auth must be auto, api-key, or chatgpt\n"
		   "config: warning: could not persist to config\n"
		   "config: note: the built-in shell tool is not enabled\n");
	free(out);
}

/* Collection preserves the order in which diagnostics were raised. */
static void test_order(struct fyai_diag *diag)
{
	char *out;
	int i;

	for (i = 0; i < 3; i++)
		fyai_error(&test_ctx, "message %d", i);

	out = drain_to_string(diag);
	expect_str("raise order",
		   out,
		   "config: message 0\n"
		   "config: message 1\n"
		   "config: message 2\n");
	free(out);
}

/* Debug is masked off until the configuration asks for it. */
static void test_mask(struct fyai_diag *diag)
{
	char *out;

	fyai_debug(&test_ctx, "invisible");
	out = drain_to_string(diag);
	expect_str("debug masked off by default", out, "");
	free(out);

	diag->mask |= 1u << FYAIET_DEBUG;
	fyai_debug(&test_ctx, "visible");
	out = drain_to_string(diag);
	expect_str("debug once unmasked", out, "config: debug: visible\n");
	free(out);
	diag->mask &= ~(1u << FYAIET_DEBUG);
}

/* The origin rides along on every diagnostic but only surfaces on request. */
static void test_source(struct fyai_diag *diag)
{
	char *out;

	diag->source = true;
	fyai_error(&test_ctx, "with origin");
	out = drain_to_string(diag);
	diag->source = false;

	if (!strstr(out, "config: with origin\n") ||
	    !strstr(out, "fyai_diag_test.c:") ||
	    !strstr(out, "test_source()")) {
		fprintf(stderr, "FAIL source origin: got %s", out);
		failures++;
	}
	free(out);
}

/* A drain leaves the sink empty rather than repeating itself. */
static void test_drain_clears(struct fyai_diag *diag)
{
	char *out;

	fyai_error(&test_ctx, "once");
	free(drain_to_string(diag));

	out = drain_to_string(diag);
	expect_str("drain clears the sink", out, "");
	free(out);
}

static void *raise_worker(void *arg)
{
	int i;

	for (i = 0; i < DIAG_PER_THREAD; i++)
		fyai_error(&test_ctx, "thread %d message %d", *(int *)arg, i);
	return NULL;
}

/*
 * The real point of the CAS: publishing an appended sequence is a
 * read-modify-write, so a plain store silently drops diagnostics whenever two
 * threads raise at once. Count the drained lines - every raise must survive.
 */
static void test_concurrent_raise(struct fyai_diag *diag)
{
	pthread_t th[DIAG_THREADS];
	int id[DIAG_THREADS];
	size_t lines;
	char *out, *p;
	int i;

	for (i = 0; i < DIAG_THREADS; i++) {
		id[i] = i;
		if (pthread_create(&th[i], NULL, raise_worker, &id[i])) {
			fprintf(stderr, "pthread_create failed\n");
			exit(1);
		}
	}
	for (i = 0; i < DIAG_THREADS; i++)
		pthread_join(th[i], NULL);

	out = drain_to_string(diag);
	for (lines = 0, p = out; (p = strchr(p, '\n')); p++)
		lines++;
	expect_eq("no diagnostic lost under contention",
		  (long long)lines, (long long)DIAG_THREADS * DIAG_PER_THREAD);
	free(out);
}

/*
 * Each drain resets the builder, so a long run of them must not accumulate.
 * Left un-reset this loop would retain every message ever raised.
 */
static void test_reset_reclaims(struct fyai_diag *diag)
{
	char *out;
	int i;

	for (i = 0; i < 10000; i++) {
		fyai_error(&test_ctx, "message %d", i);
		out = drain_to_string(diag);
		if (!*out) {
			fprintf(stderr, "FAIL reset: drain %d empty\n", i);
			failures++;
			free(out);
			return;
		}
		free(out);
	}
	expect_eq("survived repeated drain/reset", 0, 0);
}

/* A sink-less caller still reports; this is the path everything running before
 * the configuration exists takes. */
static void test_no_sink(void)
{
	fyai_diagf(NULL, FYAIET_ERROR, FYAIEM_AUTH, __FILE__, __LINE__,
		   __func__, "sink-less diagnostic reaches stderr");
}

int main(void)
{
	struct fyai_diag *diag;

	memset(&test_cfg, 0, sizeof(test_cfg));
	memset(&test_ctx, 0, sizeof(test_ctx));
	test_ctx.cfg = &test_cfg;

	if (fyai_diag_setup(&test_cfg.diag)) {
		fprintf(stderr, "fyai_diag_setup() failed\n");
		return 1;
	}
	diag = fyai_ctx_diag(&test_ctx);
	if (diag != &test_cfg.diag) {
		fprintf(stderr, "FAIL: fyai_ctx_diag() did not resolve\n");
		return 1;
	}

	test_format(diag);
	test_order(diag);
	test_mask(diag);
	test_source(diag);
	test_drain_clears(diag);
	test_concurrent_raise(diag);
	test_reset_reclaims(diag);
	test_no_sink();

	fyai_diag_cleanup(&test_cfg.diag);

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all diag tests passed\n");
	return 0;
}
