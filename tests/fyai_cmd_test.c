/*
 * fyai_cmd_test.c - unit tests for schema-defined commands
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "utils.h"
#include "fyai_config.h"

#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(cmd, registry, cmd_registry)
FYAI_TEST_ENTRY(cmd, descriptions, cmd_descriptions)
FYAI_TEST_ENTRY(cmd, split, cmd_split)
FYAI_TEST_ENTRY(cmd, parse_cli, cmd_parse_cli)
FYAI_TEST_ENTRY(cmd, parse_errors, cmd_parse_errors)
FYAI_TEST_ENTRY(cmd, parse_session, cmd_parse_session)
FYAI_TEST_ENTRY(cmd, usage, cmd_usage)
FYAI_TEST_ENTRY(cmd, help, cmd_help_text)
FYAI_TEST_ENTRY(cmd, complete, cmd_complete_words)
FYAI_TEST_ENTRY(cmd, complete_session, cmd_complete_session)
FYAI_TEST_ENTRY(cmd, complete_view, cmd_complete_view)
FYAI_TEST_ENTRY(cmd, immediate, cmd_immediate)
FYAI_TEST_ENTRY(cmd, group_args, cmd_group_args)
FYAI_TEST_ENTRY(cmd, view_session_args, cmd_view_session_args)
FYAI_TEST_ENTRY(cmd, config_args, cmd_config_args)
FYAI_TEST_ENTRY(cmd, history_args, cmd_history_args)
FYAI_TEST_ENTRY(cmd, complete_config, cmd_complete_config)
FYAI_TEST_ENTRY(cmd, complete_catalog, cmd_complete_catalog)
FYAI_TEST_ENTRY(cmd, surfaces, cmd_surfaces)
FYAI_TEST_ENTRY(cmd, setting_scope, cmd_setting_scope)

struct cmd_test {
	struct fyai_cfg cfg;
	struct fy_generic_builder *gb;
};

static void cmd_test_open(struct cmd_test *t)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	int rc;

	memset(t, 0, sizeof(*t));
	t->gb = fy_generic_builder_create(&cfg);
	FYAI_TCHECK(t->gb);
	rc = fyai_diag_setup(&t->cfg.diag);
	FYAI_TCHECK(!rc);
}

static void cmd_test_close(struct cmd_test *t)
{
	fyai_diag_cleanup(&t->cfg.diag);
	fy_generic_builder_destroy(t->gb);
}

/* The rendered diagnostics; the collection is reset. */
static char *cmd_test_diag(struct cmd_test *t)
{
	char *s;

	s = fyai_diag_string(&t->cfg.diag);
	fyai_diag_reset(&t->cfg.diag);
	return s;
}

static int cmd_test_parse(struct cmd_test *t, enum fyai_cmd_surface surface,
			  const char *line, struct fyai_cmd_parsed *out)
{
	char **words;
	size_t *offs;
	int n, rc;

	n = fyai_cmd_split(line, false, &words, &offs);
	FYAI_TCHECK(n > 0);
	rc = fyai_cmd_parse(&t->cfg, t->gb, surface, (size_t)n,
			    (const char *const *)words, line,
			    surface == FYAI_CMD_SESSION ? offs : NULL, out);
	fyai_cmd_split_free(words, n, offs);
	return rc;
}

static bool arg_is(fy_generic args, const char *key, const char *value)
{
	fy_generic v;

	v = fy_get(args, key, fy_invalid);
	return fy_is_string(v) && !strcmp(fy_castp(&v, ""), value);
}

int cmd_registry(void)
{
	fy_generic reg;

	reg = fyai_cmd_registry();
	if (!fy_is_valid(reg))
		fprintf(stderr, "%s\n", fyai_cmd_registry_why());
	FYAI_TCHECK(fy_is_valid(reg));
	FYAI_TCHECK(!*fyai_cmd_registry_why());
	FYAI_TCHECK(fyai_cmd_is_verb("branch"));
	FYAI_TCHECK(fyai_cmd_is_verb("__complete"));
	FYAI_TCHECK(!fyai_cmd_is_verb("nosuch"));
	return 0;
}

static void check_described(fy_generic def)
{
	fy_generic p, sub, v;
	const char *name;

	v = fy_get(def, "title", fy_invalid);
	FYAI_TCHECK(fy_is_string(v));
	v = fy_get(def, "description", fy_invalid);
	FYAI_TCHECK(fy_is_string(v));
	fy_foreach_key_value(name, p, fy_get(fy_get(def, "arguments",
						     fy_invalid),
					     "properties", fy_invalid)) {
		v = fy_get(p, "description", fy_invalid);
		if (!fy_is_string(v) || !*fy_castp(&v, ""))
			fprintf(stderr, "%s has no description\n", name);
		FYAI_TCHECK(fy_is_string(v));
	}
	fy_foreach(sub, fy_get(def, "commands", fy_invalid))
		check_described(sub);
}

/* Help is built from the definitions: each part must say what it is. */
int cmd_descriptions(void)
{
	fy_generic def;

	fy_foreach(def, fy_get(fyai_cmd_registry(), "commands", fy_invalid))
		check_described(def);
	return 0;
}

int cmd_split(void)
{
	char **words;
	size_t *offs;
	int n;

	n = fyai_cmd_split("a 'b c' \"d \\\"e\\\"\" f\\ g", false, &words,
			   &offs);
	FYAI_TCHECK(n == 4);
	FYAI_TCHECK(!strcmp(words[0], "a"));
	FYAI_TCHECK(!strcmp(words[1], "b c"));
	FYAI_TCHECK(!strcmp(words[2], "d \"e\""));
	FYAI_TCHECK(!strcmp(words[3], "f g"));
	FYAI_TCHECK(offs[1] == 2);
	fyai_cmd_split_free(words, n, offs);

	/* A trailing blank is the empty word that completion fills. */
	n = fyai_cmd_split("branch ", true, &words, &offs);
	FYAI_TCHECK(n == 2 && !*words[1] && offs[1] == 7);
	fyai_cmd_split_free(words, n, offs);

	n = fyai_cmd_split("branch", true, &words, &offs);
	FYAI_TCHECK(n == 1);
	fyai_cmd_split_free(words, n, offs);
	return 0;
}

int cmd_parse_cli(void)
{
	struct fyai_cmd_parsed p;
	struct cmd_test t;
	int rc;

	cmd_test_open(&t);

	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "branch new feat main~2", &p);
	FYAI_TCHECK(!rc && !p.help);
	FYAI_TCHECK(!strcmp(p.path, "branch new"));
	FYAI_TCHECK(arg_is(p.args, "name", "feat"));
	FYAI_TCHECK(arg_is(p.args, "ref", "main~2"));

	/* An alias names the same command. */
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "branch create x", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "branch new"));

	/* No subcommand runs the default; an option goes to it. */
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "branch -a", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "branch list"));
	FYAI_TCHECK(fy_get(p.args, "all", false));

	/* An unknown word goes to the fallback of the CLI: a list below it. */
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "branch feature", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "branch list"));
	FYAI_TCHECK(arg_is(p.args, "under", "feature"));

	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "branch delete --force x", &p);
	FYAI_TCHECK(!rc && fy_get(p.args, "force", false));

	rc = cmd_test_parse(&t, FYAI_CMD_CLI,
			    "checkout -b fix main@{1} --output=json", &p);
	FYAI_TCHECK(!rc && fy_get(p.args, "create", false));
	FYAI_TCHECK(arg_is(p.args, "target", "fix"));
	FYAI_TCHECK(arg_is(p.args, "start", "main@{1}"));
	FYAI_TCHECK(p.format == FYAI_CMD_OUT_JSON);

	/* A rest argument on the CLI joins the words. */
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "branch describe x one two", &p);
	FYAI_TCHECK(!rc && arg_is(p.args, "text", "one two"));

	rc = cmd_test_parse(&t, FYAI_CMD_CLI,
			    "view enter demo printf '%s' 'two words' --help "
			    "--output=json", &p);
	FYAI_TCHECK(!rc && !p.help && p.format == FYAI_CMD_OUT_MARKDOWN);
	FYAI_TCHECK(fy_equal(fy_get(p.args, "command", fy_invalid),
			     fy_sequence("printf", "%s", "two words", "--help",
					 "--output=json")));
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "view enter demo", &p);
	FYAI_TCHECK(!rc && !p.help);
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "view enter demo -- sh -c true", &p);
	FYAI_TCHECK(!rc && fy_equal(fy_get(p.args, "command", fy_invalid),
		fy_sequence("sh", "-c", "true")));

	/* A variadic positional takes every word; -- ends the options. */
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "__complete -- branch -a ''", &p);
	FYAI_TCHECK(!rc && fy_len(fy_get(p.args, "words", fy_invalid)) == 3);

	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "branch new --help", &p);
	FYAI_TCHECK(!rc && p.help && !strcmp(p.path, "branch new"));
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "branch --help", &p);
	FYAI_TCHECK(!rc && p.help && !strcmp(p.path, "branch"));

	FYAI_TCHECK(!fyai_diag_got_error(&t.cfg.diag));
	cmd_test_close(&t);
	return 0;
}

static void expect_error(struct cmd_test *t, enum fyai_cmd_surface s,
			 const char *line, const char *text)
{
	struct fyai_cmd_parsed p;
	char *msg;
	int rc;

	rc = cmd_test_parse(t, s, line, &p);
	FYAI_TCHECK(rc);
	msg = cmd_test_diag(t);
	if (!msg || !strstr(msg, text))
		fprintf(stderr, "'%s': expected '%s', got '%s'\n", line, text,
			msg ? msg : "");
	FYAI_TCHECK(msg && strstr(msg, text));
	free(msg);
}

int cmd_parse_errors(void)
{
	struct cmd_test t;

	cmd_test_open(&t);
	expect_error(&t, FYAI_CMD_CLI, "branch new",
		     "branch new: NAME is required; usage: fyai branch new "
		     "NAME [REF]");
	expect_error(&t, FYAI_CMD_CLI, "branch new a b c",
		     "unexpected argument 'c'");
	expect_error(&t, FYAI_CMD_CLI, "branch delete --nope x",
		     "unknown option '--nope'");
	expect_error(&t, FYAI_CMD_CLI, "branch delete -f -f x",
		     "-f is given more than once");
	expect_error(&t, FYAI_CMD_CLI, "branch list --output xml",
		     "--output takes markdown, json, or yaml");
	expect_error(&t, FYAI_CMD_CLI, "completion tcsh", "completion:");
	/* The CLI takes no prefix. */
	expect_error(&t, FYAI_CMD_CLI, "bra", "unknown command 'bra'");
	/* A session-only command is not a verb. */
	expect_error(&t, FYAI_CMD_CLI, "branch switch x",
		     "unexpected argument 'x'");
	cmd_test_close(&t);
	return 0;
}

int cmd_parse_session(void)
{
	struct fyai_cmd_parsed p;
	struct cmd_test t;
	int rc;

	cmd_test_open(&t);
	/* The session takes a unique prefix of a command and a subcommand. */
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "branch ne x", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "branch new"));
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "chec main", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "checkout"));
	/* A prefix of two commands names neither. */
	expect_error(&t, FYAI_CMD_SESSION, "bra", "unknown command 'bra'");

	/* An unknown word switches the session. */
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "branch feature", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "branch switch"));
	FYAI_TCHECK(arg_is(p.args, "name", "feature"));

	/* A rest argument keeps the line as typed. */
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION,
			    "branch describe x  it's  here ", &p);
	FYAI_TCHECK(!rc && arg_is(p.args, "text", "it's  here"));

	/* --output is a verb option. */
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "model --output json", &p);
	FYAI_TCHECK(rc);
	fyai_diag_reset(&t.cfg.diag);
	cmd_test_close(&t);
	return 0;
}

int cmd_usage(void)
{
	struct fyai_cmd_walk w;
	const char *words[] = { "checkout" };
	char *u;
	int rc;

	rc = fyai_cmd_walk(FYAI_CMD_CLI, 1, words, 1, &w);
	FYAI_TCHECK(!rc);
	u = fyai_cmd_usage(w.def, w.path, FYAI_CMD_CLI);
	FYAI_TCHECK(u && !strcmp(u, "fyai checkout [-b] BRANCH [START]"));
	free(u);
	u = fyai_cmd_usage(w.def, w.path, FYAI_CMD_SESSION);
	FYAI_TCHECK(u && !strcmp(u, "/checkout [-b] BRANCH [START]"));
	free(u);
	return 0;
}

int cmd_help_text(void)
{
	struct response_buffer out = { 0 };
	const char *path[] = { "branch", "new" };
	const char *topic[] = { "refs" };
	struct cmd_test t;
	int rc;

	cmd_test_open(&t);
	rc = fyai_cmd_help_source(&t.cfg, FYAI_CMD_CLI, 2, path, &out);
	FYAI_TCHECK(!rc && out.data);
	FYAI_TCHECK(strstr(out.data, "## fyai branch new"));
	FYAI_TCHECK(strstr(out.data, "`fyai branch new NAME [REF]`"));
	FYAI_TCHECK(strstr(out.data, "**Aliases:** `create`"));
	FYAI_TCHECK(strstr(out.data, "--output FORMAT"));
	out.len = 0;
	rc = fyai_cmd_help_source(&t.cfg, FYAI_CMD_SESSION, 2, path, &out);
	FYAI_TCHECK(!rc && strstr(out.data, "## /branch new"));
	FYAI_TCHECK(!strstr(out.data, "--output FORMAT"));
	out.len = 0;
	rc = fyai_cmd_help_source(&t.cfg, FYAI_CMD_CLI, 1, topic, &out);
	FYAI_TCHECK(!rc && strstr(out.data, "the reference syntax"));
	free(out.data);
	cmd_test_close(&t);
	return 0;
}

struct cands {
	char buf[4096];
	size_t len;
};

static void cands_add(void *arg, const char *value, const char *desc)
{
	struct cands *c = arg;
	int n;

	(void)desc;
	n = snprintf(c->buf + c->len, sizeof(c->buf) - c->len, "%s\n", value);
	if (n > 0 && (size_t)n < sizeof(c->buf) - c->len)
		c->len += (size_t)n;
}

static unsigned int complete(enum fyai_cmd_surface s, const char *line,
			     struct cands *c)
{
	char **words;
	size_t *offs;
	unsigned int d;
	int n;

	memset(c, 0, sizeof(*c));
	n = fyai_cmd_split(line, true, &words, &offs);
	FYAI_TCHECK(n > 0);
	d = fyai_cmd_complete(NULL, s, (size_t)n, (const char *const *)words,
			      cands_add, c);
	fyai_cmd_split_free(words, n, offs);
	return d;
}

int cmd_complete_view(void)
{
	struct cmd_test t;
	struct fyai_ctx ctx = { 0 };
	struct cands c;
	fy_generic views;
	const char *commands[] = { "show", "update", "diff", "remove",
				   "sync", "mount", "unmount", "enter" };
	const char *words[] = { "view", NULL, "te" };
	size_t i;

	cmd_test_open(&t);
	ctx.cfg = &t.cfg;
	views = fy_mapping(t.gb, "test-view",
			   fy_mapping(t.gb, "project", "/project"),
			   "other-view", fy_mapping(t.gb, "project", "/other"),
			   "test-invalid", 42LL);
	ctx.branch_prev =
		fy_mapping(t.gb, "store", fy_mapping(t.gb, "views", views));
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		words[1] = commands[i];
		memset(&c, 0, sizeof(c));
		fyai_cmd_complete(&ctx, FYAI_CMD_CLI, 3, words, cands_add, &c);
		FYAI_TCHECK(!strcmp(c.buf, "test-view\n"));
	}
	words[1] = "create";
	memset(&c, 0, sizeof(c));
	fyai_cmd_complete(&ctx, FYAI_CMD_CLI, 3, words, cands_add, &c);
	FYAI_TCHECK(!strstr(c.buf, "test-view"));
	words[1] = "enter";
	memset(&c, 0, sizeof(c));
	fyai_cmd_complete(NULL, FYAI_CMD_CLI, 3, words, cands_add, &c);
	FYAI_TCHECK(!c.len);
	ctx.branch_prev = fy_mapping(
		t.gb, "store",
		fy_mapping(t.gb, "views",
			   fy_mapping(t.gb, "team-view",
				      fy_mapping(t.gb, "project", "/team"))));
	memset(&c, 0, sizeof(c));
	fyai_cmd_complete(&ctx, FYAI_CMD_CLI, 3, words, cands_add, &c);
	FYAI_TCHECK(!strcmp(c.buf, "team-view\n"));
	cmd_test_close(&t);
	return 0;
}

int cmd_complete_words(void)
{
	struct cands c;
	unsigned int d;

	complete(FYAI_CMD_CLI, "br", &c);
	FYAI_TCHECK(!strcmp(c.buf, "branch\n"));
	/* A hidden command is not offered. */
	complete(FYAI_CMD_CLI, "__", &c);
	FYAI_TCHECK(!c.len);
	complete(FYAI_CMD_CLI, "branch ", &c);
	FYAI_TCHECK(strstr(c.buf, "new\n") && strstr(c.buf, "delete\n"));
	/* A session-only subcommand is not a verb. */
	FYAI_TCHECK(!strstr(c.buf, "switch\n"));
	complete(FYAI_CMD_CLI, "branch delete --", &c);
	FYAI_TCHECK(strstr(c.buf, "--force\n") && strstr(c.buf, "--output\n"));
	complete(FYAI_CMD_CLI, "branch delete -f --", &c);
	FYAI_TCHECK(!strstr(c.buf, "--force\n"));
	complete(FYAI_CMD_CLI, "branch show --output ", &c);
	FYAI_TCHECK(!strcmp(c.buf, "markdown\njson\nyaml\n"));
	complete(FYAI_CMD_CLI, "completion z", &c);
	FYAI_TCHECK(!strcmp(c.buf, "zsh\n"));
	/* A layout is auto or a layout of the page document. */
	complete(FYAI_CMD_SESSION, "layout ", &c);
	FYAI_TCHECK(!strcmp(c.buf, "auto\nside\nband\n"));
	/* Global options come before the verb. */
	complete(FYAI_CMD_CLI, "--color o", &c);
	FYAI_TCHECK(!strcmp(c.buf, "off\non\n"));
	complete(FYAI_CMD_CLI, "--color on bra", &c);
	FYAI_TCHECK(!strcmp(c.buf, "branch\n"));
	/* A configuration path goes on in the same word. */
	d = complete(FYAI_CMD_CLI, "--set displa", &c);
	FYAI_TCHECK(!strcmp(c.buf, "display/\n"));
	FYAI_TCHECK(d & FYAI_CMD_COMPLETE_NOSPACE);
	d = complete(FYAI_CMD_CLI, "--config ", &c);
	FYAI_TCHECK(d & FYAI_CMD_COMPLETE_FILES);
	complete(FYAI_CMD_CLI, "help re", &c);
	FYAI_TCHECK(strstr(c.buf, "refs\n") && strstr(c.buf, "reset\n"));
	/* A help path goes on with the subcommands of the group it names. */
	complete(FYAI_CMD_CLI, "help config ", &c);
	FYAI_TCHECK(strstr(c.buf, "set\n") && strstr(c.buf, "show\n"));
	FYAI_TCHECK(!strstr(c.buf, "branch\n") && !strstr(c.buf, "refs\n"));
	complete(FYAI_CMD_CLI, "help config s", &c);
	FYAI_TCHECK(strstr(c.buf, "set\n") && !strstr(c.buf, "get\n"));
	/* A command without subcommands, or a topic, ends the path. */
	complete(FYAI_CMD_CLI, "help branch new ", &c);
	FYAI_TCHECK(!c.len);
	complete(FYAI_CMD_CLI, "help refs ", &c);
	FYAI_TCHECK(!c.len);
	return 0;
}

int cmd_complete_session(void)
{
	struct cands c;

	memset(&c, 0, sizeof(c));
	fyai_cmd_session_complete(NULL, "/bra", cands_add, &c);
	FYAI_TCHECK(!strcmp(c.buf, "/branch\n/branches\n"));
	memset(&c, 0, sizeof(c));
	fyai_cmd_session_complete(NULL, "/branch  sw", cands_add, &c);
	FYAI_TCHECK(!strcmp(c.buf, "/branch  switch\n"));
	/* A value comes from the schema of the item the key names. */
	memset(&c, 0, sizeof(c));
	fyai_cmd_session_complete(NULL, "/config set mcp/enabled ", cands_add,
				  &c);
	FYAI_TCHECK(!strcmp(c.buf, "/config set mcp/enabled true\n"
			    "/config set mcp/enabled false\n"));
	/* The popup of the session completes a help path the same way. */
	memset(&c, 0, sizeof(c));
	fyai_cmd_session_complete(NULL, "/help config se", cands_add, &c);
	FYAI_TCHECK(!strcmp(c.buf, "/help config set\n"));
	/* --output is not a session option. */
	memset(&c, 0, sizeof(c));
	fyai_cmd_session_complete(NULL, "/branch delete --o", cands_add, &c);
	FYAI_TCHECK(!c.len);
	return 0;
}

int cmd_immediate(void)
{
	FYAI_TCHECK(fyai_cmd_session_immediate("branch"));
	FYAI_TCHECK(fyai_cmd_session_immediate("branch list -a"));
	FYAI_TCHECK(fyai_cmd_session_immediate("help branch"));
	FYAI_TCHECK(!fyai_cmd_session_immediate("branch new x"));
	FYAI_TCHECK(!fyai_cmd_session_immediate("branch feature"));
	FYAI_TCHECK(fyai_cmd_session_immediate("model"));
	FYAI_TCHECK(!fyai_cmd_session_immediate("model gpt-5"));
	FYAI_TCHECK(!fyai_cmd_session_immediate("checkout main"));
	return 0;
}

/* A word that names no subcommand fills the next argument of the group. */
int cmd_group_args(void)
{
	struct fyai_cmd_parsed p;
	struct cmd_test t;
	struct cands c;
	int rc;

	cmd_test_open(&t);
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "auth openai info --output json",
			    &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "auth info"));
	FYAI_TCHECK(arg_is(p.args, "provider", "openai"));
	FYAI_TCHECK(p.format == FYAI_CMD_OUT_JSON);

	/* The default of the group applies with no word. */
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "auth", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "auth status"));
	FYAI_TCHECK(arg_is(p.args, "provider", "openai"));

	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "auth anthropic", &p);
	FYAI_TCHECK(!rc && arg_is(p.args, "provider", "anthropic"));

	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "auth login --manual", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "auth login"));
	FYAI_TCHECK(fy_get(p.args, "manual", false));
	cmd_test_close(&t);

	complete(FYAI_CMD_CLI, "auth ", &c);
	FYAI_TCHECK(strstr(c.buf, "login\n") && strstr(c.buf, "status\n"));
	return 0;
}

int cmd_view_session_args(void)
{
	struct fyai_cmd_parsed p;
	struct cmd_test t;
	int rc;

	cmd_test_open(&t);
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "view", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "view list"));
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "view list --full", &p);
	FYAI_TCHECK(!rc && fy_get(p.args, "full", false));
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION,
			    "view enter demo sh -c 'printf two words'", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "view enter"));
	FYAI_TCHECK(arg_is(p.args, "name", "demo"));
	FYAI_TCHECK(fy_len(fy_get(p.args, "command")) == 3);
	FYAI_TCHECK(fy_equal(fy_get_at(fy_get(p.args, "command"), 2),
			     "printf two words"));
	cmd_test_close(&t);
	return 0;
}

int cmd_config_args(void)
{
	struct fyai_cmd_parsed p;
	struct cmd_test t;
	int rc;

	cmd_test_open(&t);
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "config", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "config show"));
	/* The value is the rest of the line, as a YAML flow document. */
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION,
			    "config set sandbox { enabled: true }", &p);
	FYAI_TCHECK(!rc && arg_is(p.args, "value", "{ enabled: true }"));
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "sandbox enable", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "sandbox on"));
	/* In a session, /sandbox is the setting, not this command. */
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "sandbox on", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "sandbox"));
	FYAI_TCHECK(arg_is(p.args, "value", "on"));
	expect_error(&t, FYAI_CMD_CLI, "config get", "KEY is required");
	cmd_test_close(&t);
	return 0;
}

int cmd_history_args(void)
{
	struct fyai_cmd_parsed p;
	struct cmd_test t;
	fy_generic sel;
	int rc;

	cmd_test_open(&t);
	rc = cmd_test_parse(&t, FYAI_CMD_CLI, "transcript --last 2 --raw", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "history"));
	FYAI_TCHECK(fy_get(p.args, "last", 0LL) == 2);
	FYAI_TCHECK(fy_get(p.args, "raw", false));
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION,
			    "history --tool-detail=brief last 2", &p);
	FYAI_TCHECK(!rc && arg_is(p.args, "tool_detail", "brief"));
	sel = fy_get(p.args, "select", fy_invalid);
	FYAI_TCHECK(fy_len(sel) == 2);
	expect_error(&t, FYAI_CMD_CLI, "history --range 1-2",
		     "history:");
	expect_error(&t, FYAI_CMD_CLI, "history --tool-detail=lots",
		     "history:");
	/* --raw is a verb option. */
	expect_error(&t, FYAI_CMD_SESSION, "history --raw",
		     "unknown option '--raw'");
	cmd_test_close(&t);
	return 0;
}

int cmd_complete_config(void)
{
	struct cands c;
	unsigned int d;

	d = complete(FYAI_CMD_CLI, "config get displ", &c);
	FYAI_TCHECK(!strcmp(c.buf, "display/\n"));
	FYAI_TCHECK(d & FYAI_CMD_COMPLETE_NOSPACE);
	/* The value follows the item that the key names. */
	complete(FYAI_CMD_CLI, "config set reasoning/effort m", &c);
	FYAI_TCHECK(strstr(c.buf, "medium\n") && strstr(c.buf, "minimal\n"));
	complete(FYAI_CMD_CLI, "config set display/markdown ", &c);
	FYAI_TCHECK(!strcmp(c.buf, "true\nfalse\n"));
	complete(FYAI_CMD_CLI, "history ", &c);
	FYAI_TCHECK(strstr(c.buf, "last\n"));
	return 0;
}

/* Catalogue paths follow the document; values follow its schema. */
int cmd_complete_catalog(void)
{
	struct cands c;
	unsigned int d;

	d = complete(FYAI_CMD_CLI, "catalog set prov", &c);
	FYAI_TCHECK(!strcmp(c.buf, "providers/\n"));
	FYAI_TCHECK(d & FYAI_CMD_COMPLETE_NOSPACE);
	complete(FYAI_CMD_CLI, "catalog get providers/open", &c);
	FYAI_TCHECK(strstr(c.buf, "providers/openai/\n"));
	complete(FYAI_CMD_CLI, "catalog get providers/openai/ro", &c);
	FYAI_TCHECK(!strcmp(c.buf, "providers/openai/root_url\n"));
	complete(FYAI_CMD_CLI, "catalog set models/gpt-5.4-mini/modalities/0 ",
		 &c);
	FYAI_TCHECK(strstr(c.buf, "text\n") && strstr(c.buf, "image\n"));
	return 0;
}

/* One command can offer different subcommands on each surface. */
int cmd_surfaces(void)
{
	struct fyai_cmd_parsed p;
	struct cmd_test t;
	int rc;

	cmd_test_open(&t);
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "mcp", &p);
	FYAI_TCHECK(!rc && !strcmp(p.path, "mcp status"));
	rc = cmd_test_parse(&t, FYAI_CMD_SESSION, "mcp login github", &p);
	FYAI_TCHECK(!rc && arg_is(p.args, "name", "github"));
	rc = cmd_test_parse(&t, FYAI_CMD_CLI,
			    "mcp oauth import-client gh c.json --endpoint "
			    "https://x --scope a --scope b", &p);
	FYAI_TCHECK(!rc && fy_len(fy_get(p.args, "scope", fy_invalid)) == 2);
	expect_error(&t, FYAI_CMD_CLI, "mcp", "a subcommand is required");
	expect_error(&t, FYAI_CMD_CLI, "mcp login github",
		     "unknown subcommand 'login'");
	expect_error(&t, FYAI_CMD_CLI, "mcp oauth import-client gh c.json "
		     "--endpoint https://x", "--scope is required");
	cmd_test_close(&t);
	return 0;
}

/* The schema scopes a setting: the nearest x-fyai-scope on the path. */
int cmd_setting_scope(void)
{
	struct cands c;

	FYAI_TCHECK(fyai_config_session_scoped("display/markdown"));
	FYAI_TCHECK(fyai_config_session_scoped("display/stream"));
	FYAI_TCHECK(fyai_config_session_scoped("sandbox/enabled"));
	FYAI_TCHECK(fyai_config_session_scoped("token_extents"));
	FYAI_TCHECK(!fyai_config_session_scoped("display/theme"));
	FYAI_TCHECK(!fyai_config_session_scoped("temperature"));
	FYAI_TCHECK(!fyai_config_session_scoped("reasoning/effort"));
	FYAI_TCHECK(!fyai_config_session_scoped("no/such/key"));

	/* A setting completes from the schema of its key. */
	memset(&c, 0, sizeof(c));
	fyai_cmd_session_complete(NULL, "/effort m", cands_add, &c);
	FYAI_TCHECK(strstr(c.buf, "/effort medium\n"));
	memset(&c, 0, sizeof(c));
	fyai_cmd_session_complete(NULL, "/markdown o", cands_add, &c);
	FYAI_TCHECK(!strcmp(c.buf, "/markdown on\n/markdown off\n"));
	return 0;
}
