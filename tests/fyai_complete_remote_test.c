/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_branch_state.h"
#include "fyai_cmd.h"
#include "fyai_query.h"
#include "fyai_screen.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "fyai_test_diag.h"
#include "fyai_uimodel.h"
#include "fyai_uistores.h"

FYAI_TEST_ENTRY(complete_remote, worker_answer, complete_worker_answer)
FYAI_TEST_ENTRY(complete_remote, worker_refuses, complete_worker_refuses)
FYAI_TEST_ENTRY(complete_remote, client_cache, complete_client_cache)
FYAI_TEST_ENTRY(complete_remote, client_model_slash, complete_client_model_slash)
FYAI_TEST_ENTRY(complete_remote, client_local, complete_client_local)
FYAI_TEST_ENTRY(complete_remote, reset, complete_client_reset)

/* The worker: a branch table. The client: a mirror whose owner is the worker. */
struct fixture {
	struct fy_generic_builder *gb;
	struct fyai_branch_state worker_state, client_state;
	struct fyai_ctx worker, client;
	struct fyai_screen screen;
	struct fyai_uistores stores;
	struct fyai_cfg cfg;
	int asked;			/* the queries that the owner got */
	bool silent;			/* the owner does not answer */
	fyai_uimodel_answer_fn held_fn;
	void *held_arg;
};

static int owner_start(void *data, const char *subtype, fy_generic request,
		       fyai_uimodel_answer_fn fn, void *arg,
		       unsigned long long *idp)
{
	struct fixture *f = data;
	struct fy_generic_builder *gb;
	fy_generic answer;

	(void)subtype;
	f->asked++;
	*idp = 0;
	if (f->silent) {
		f->held_fn = fn;
		f->held_arg = arg;
		*idp = 7;
		return 0;
	}
	gb = fy_generic_builder_create(NULL);
	answer = fyai_query(&f->worker, gb, request);
	fn(arg, answer);
	fy_generic_builder_destroy(gb);
	return 0;
}

static void owner_cancel(void *data, unsigned long long id)
{
	struct fixture *f = data;

	if (id == 7)
		f->held_fn = NULL;
}

static const struct fyai_uimodel_query_ops owner_ops = {
	.start = owner_start,
	.cancel = owner_cancel,
};

static int owner_intent(void *data, const char *subtype, fy_generic request)
{
	(void)data;
	(void)subtype;
	(void)request;
	return 0;
}

static fy_generic entry_new(struct fy_generic_builder *gb, const char *what)
{
	struct fyai_branch b;

	fyai_branch_decode(fy_invalid, &b);
	b.created = fy_value(gb, 1LL);
	b.updated = fy_value(gb, 1LL);
	b.op = fy_value(gb, FYAI_BRANCH_OP_TURN);
	b.description = fy_value(gb, what);
	return fyai_branch_build(gb, &b);
}

static int fixture_open(struct fixture *f)
{
	fy_generic table;

	memset(f, 0, sizeof(*f));
	f->gb = fy_generic_builder_create(NULL);
	FYAI_TCHECK(f->gb);
	table = fyai_branches_set(f->gb, fy_invalid, "main",
				  entry_new(f->gb, "the main line"));
	table = fyai_branches_set(f->gb, table, "main/alpha",
				  entry_new(f->gb, "alpha work"));
	table = fyai_branches_set(f->gb, table, "main/beta",
				  entry_new(f->gb, "beta work"));
	f->worker_state.branches = table;
	f->worker.bstate = &f->worker_state;
	f->cfg.catalog = fy_invalid;
	f->worker.cfg = &f->cfg;
	f->worker.screen = &f->screen;
	f->client.bstate = &f->client_state;
	f->client.cfg = &f->cfg;
	f->client.screen = &f->screen;
	f->client.uistores = &f->stores;
	FYAI_TCHECK(!fyai_uimodel_create(&f->client));
	FYAI_TCHECK(!fyai_uimodel_mirror(&f->client, owner_intent, f));
	fyai_uimodel_mirror_queries(&f->client, &owner_ops, f);
	return 0;
}

static void fixture_close(struct fixture *f)
{
	fyai_cmd_complete_reset(&f->client);
	fyai_uimodel_release(&f->client);
	fy_generic_builder_destroy(f->gb);
}

struct cands {
	char buf[2048];
	size_t len;
};

static void cands_add(void *arg, const char *value, const char *desc)
{
	struct cands *c = arg;

	(void)desc;
	c->len += (size_t)snprintf(c->buf + c->len, sizeof(c->buf) - c->len,
				   "%s\n", value);
}

/* `/checkout PARTIAL`: the argument is a reference, which reads the branches. */
static void complete_line(struct fyai_ctx *ctx, const char *partial,
			  struct cands *c)
{
	const char *words[] = { "checkout", partial };

	memset(c, 0, sizeof(*c));
	fyai_cmd_complete(ctx, FYAI_CMD_SESSION, 2, words, cands_add, c);
}

int complete_worker_answer(void)
{
	struct fixture f;
	struct fy_generic_builder *gb;
	fy_generic answer, rows;
	size_t i;

	fixture_open(&f);
	gb = fy_generic_builder_create(NULL);
	FYAI_TCHECK(gb);
	answer = fyai_query(&f.worker, gb, fy_mapping(gb, "what", "complete",
		"params", fy_mapping(gb, "kind", "branch", "partial", "main/")));
	rows = fy_get(answer, "rows", fy_invalid);
	/* The candidates that the partial word names, with their descriptions. */
	FYAI_TCHECK(fy_len(rows) == 2);
	for (i = 0; i < 2; i++)
		FYAI_TCHECK(!strncmp(fy_get(fy_get_at(rows, i), "value", ""),
				     "main/", 5));
	FYAI_TCHECK(fy_is_null(fy_get(answer, "next", fy_invalid)));
	FYAI_TCHECK(fy_get(answer, "total", 0LL) == 2);
	fy_generic_builder_destroy(gb);
	fixture_close(&f);
	return 0;
}

int complete_worker_refuses(void)
{
	struct fixture f;
	struct fy_generic_builder *gb;
	fy_generic answer;
	const char *kinds[] = { "theme", "config-path", "page-layout", "nothing" };
	size_t i;

	fixture_open(&f);
	gb = fy_generic_builder_create(NULL);
	FYAI_TCHECK(gb);
	/* A kind that reads no data of the worker is not asked of it. */
	for (i = 0; i < ARRAY_SIZE(kinds); i++) {
		answer = fyai_query(&f.worker, gb, fy_mapping(gb, "what",
			"complete", "params", fy_mapping(gb, "kind", kinds[i],
							 "partial", "")));
		FYAI_TCHECK(!strcmp(fy_get(fy_get(answer, "error", fy_invalid),
					   "code", ""), FYAI_QUERY_UNSUPPORTED));
	}
	fy_generic_builder_destroy(gb);
	fixture_close(&f);
	return 0;
}

int complete_client_cache(void)
{
	struct fixture f;
	struct cands c;

	fixture_open(&f);
	/* The owner answers at once, so the first ask is served. */
	complete_line(&f.client, "main/a", &c);
	FYAI_TCHECK(f.asked == 1);
	FYAI_TCHECK(strstr(c.buf, "main/alpha\n") && !strstr(c.buf, "main/beta"));
	/* A longer word is filtered here: nothing is asked. */
	complete_line(&f.client, "main/al", &c);
	FYAI_TCHECK(f.asked == 1);
	FYAI_TCHECK(!strcmp(c.buf, "main/alpha\n"));
	/* A word that is not within the first one asks again. */
	complete_line(&f.client, "main/b", &c);
	FYAI_TCHECK(f.asked == 2);
	FYAI_TCHECK(strstr(c.buf, "main/beta\n"));
	fixture_close(&f);
	return 0;
}

int complete_client_model_slash(void)
{
	struct fixture f;
	struct cands c;
	const char *bare[] = { "model", "" };
	const char *slash[] = { "model", "anthropic/" };

	fixture_open(&f);
	memset(&c, 0, sizeof(c));
	fyai_cmd_complete(&f.client, FYAI_CMD_SESSION, 2, bare, cands_add, &c);
	FYAI_TCHECK(f.asked == 1);
	/* The set of a bare word has no provider/model rows: ask again. */
	memset(&c, 0, sizeof(c));
	fyai_cmd_complete(&f.client, FYAI_CMD_SESSION, 2, slash, cands_add, &c);
	FYAI_TCHECK(f.asked == 2);
	FYAI_TCHECK(strstr(c.buf, "anthropic/claude"));
	fixture_close(&f);
	return 0;
}

int complete_client_local(void)
{
	struct fixture f;
	struct cands c;
	const char *words[] = { "theme", "" };

	fixture_open(&f);
	/* The commands come from the registry here; only the skills that run as
	 * commands are the worker's, so a word of a command asks for them. */
	memset(&c, 0, sizeof(c));
	fyai_cmd_complete(&f.client, FYAI_CMD_SESSION, 1, words, cands_add, &c);
	FYAI_TCHECK(strstr(c.buf, "theme\n"));
	FYAI_TCHECK(f.asked == 1);
	/* The value of a local kind asks nothing. */
	memset(&c, 0, sizeof(c));
	fyai_cmd_complete(&f.client, FYAI_CMD_SESSION, 2, words, cands_add, &c);
	FYAI_TCHECK(strstr(c.buf, "dark\n"));
	FYAI_TCHECK(f.asked == 1);
	fixture_close(&f);
	return 0;
}

int complete_client_reset(void)
{
	struct fixture f;
	struct cands c;

	fixture_open(&f);
	/* A silent owner: the first ask finds none, and a second ask of the
	 * same word does not send another. */
	f.silent = true;
	complete_line(&f.client, "main/", &c);
	FYAI_TCHECK(!c.len && f.asked == 1);
	complete_line(&f.client, "main/a", &c);
	FYAI_TCHECK(!c.len && f.asked == 1);
	/* A new line forgets it, and cancels the query that waits. */
	fyai_cmd_complete_reset(&f.client);
	FYAI_TCHECK(!f.held_fn);
	complete_line(&f.client, "main/", &c);
	FYAI_TCHECK(f.asked == 2);
	fixture_close(&f);
	return 0;
}
