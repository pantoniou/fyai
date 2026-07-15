/*
 * commands.c - fyai verb dispatch and colorized usage
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The usage printer follows the style of libfyaml's fy-tool.c: an
 * isatty-gated colorizer plus padded "option : description" items grouped
 * into sections.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "commands.h"
#include "fyai.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_log.h"
#include "fyai_terminal.h"
#include "fyai_display.h"
#include "fyai_render.h"
#include "fyai_markdown.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_tools.h"
#include "utils.h"

void fyai_usage(FILE *fp, const char *progname, const char *color_mode)
{
	bool color = ansi_color_on(color_mode, fileno(fp));
	const char *h = color ? FYAI_ANSI_BRIGHT_YELLOW : "";
	const char *x = color ? FYAI_ANSI_CYAN : "";
	const char *r = color ? FYAI_ANSI_RESET : "";

#define SECTION(t) fprintf(fp, "\n%s%s%s:\n", h, (t), r)
#define ITEM(o, d) usage_item(fp, color, (o), (d))

	fprintf(fp, "%sUsage%s: %s%s%s [global-options] [verb] [args]\n",
		h, r, x, progname, r);
	fprintf(fp, "       %s%s%s [global-options] <prompt...>\n",
		x, progname, r);

	SECTION("Verbs");
	ITEM("init [path]", "Create ./.fyai (copy [path] to its config.yaml)");
	ITEM("dump [target]", "Dump state (state|anchors|providers)");
	ITEM("history [opts]", "Human-digestible conversation history");
	ITEM("display [opts]", "Alias for history");
	ITEM("stats [--raw|--json|--yaml]", "Report cumulative token/cost usage");
	ITEM("config [args]", "show|get|set|delete|edit|import|export");
	ITEM("list [what]", "List providers, models, turns, exchanges, or reflog (--brief|--full)");
	ITEM("clear", "Reset the conversation (publish a null head)");
	ITEM("compact [hint]", "Summarize history into a fresh chain");
	ITEM("context", "Context fill and token estimate");
	ITEM("api [mode]", "Show or persist the API grammar");
	ITEM("catalog tools [agent] [--brief|--full]", "List catalog agent tools and descriptions");
	ITEM("log [target] [action]", "Control wire/conversation trace logs");
	ITEM("sandbox [cmd]", "Show or configure the stored sandbox policy");
	ITEM("gc", "Garbage-collect the arena");
	ITEM("tool <name> [json]", "Run one tool sandboxed (read_file|shell|...)");
	ITEM("help [verb]", "Describe all verbs, or one verb in detail");
	fprintf(fp, "  %swith no verb, the arguments are run as a prompt%s\n",
		"", "");

	SECTION("Global options");
	ITEM("--config, -C <file>", "Load an explicit config file");
	ITEM("--env, -e <file>", "Source a .env file (used vars only)");
	ITEM("--model, -m <model>", "Model, optionally provider/model");
	ITEM("--set <key[=val]>", "Set a config key (slash path); repeatable");
	ITEM("--get <key>", "Print a config key as one-line flow");
	ITEM("--delete <key>", "Delete a config key; repeatable");
	ITEM("--transient", "Keep this session's edits/state in memory only");
	ITEM("--api-key, -k <key>", "API key (else PROVIDER_API_KEY env)");
	ITEM("--url, -u <url>", "API endpoint URL");
	ITEM("--new", "Start a new conversation");
	ITEM("--tools", "Enable function tools");
	ITEM("--sandbox", "Landlock-confine shell tools (Linux)");
	ITEM("--color <c>", "Colour output: auto|off|on");
	ITEM("--theme <t>", "Markdown theme: auto|dark|light");
	ITEM("--code-theme <t>", "Fenced-code theme: a libfyts name or a path");
	ITEM("--cache-info, -c", "Print provider cache info");
	ITEM("--interactive, -i", "Interactive prompt loop");
	ITEM("--answer <text>", "Pre-supply an ask_user answer (repeatable)");
	ITEM("--debug, -d", "Increase debug verbosity");
	fprintf(fp, "  %severything else (temperature, reasoning, streaming,%s\n",
		"", "");
	fprintf(fp, "  %smarkdown rendering, stats, logprobs, ...) is a config key -%s\n",
		"", "");
	fprintf(fp, "  %s`config set <key> <val>` / `--set <key>=<val>`%s\n",
		"", "");

#undef SECTION
#undef ITEM
}

/* ---- verbs ------------------------------------------------------------- */

int fyai_run(struct fyai_cfg *cfg)
{
	struct fyai_ctx ctx;
	int rc;

	rc = fyai_setup(&ctx, cfg);
	if (rc) {
		fprintf(stderr, "Failed to initialize fyai\n");
		goto err_out;
	}

	/* Persist/replay any global --set/--delete/--get before the verb runs. */
	rc = fyai_apply_config_ops(&ctx);
	if (rc) {
		fprintf(stderr, "config operation failed\n");
		goto err_out;
	}

	rc = fyai_execute(&ctx);
	if (rc) {
		fprintf(stderr, "fyai execution failed\n");
		goto err_out;
	}

out:
	fyai_cleanup(&ctx);
	return rc ? -1 : 0;

err_out:
	rc = -1;
	goto out;
}

static int parse_turn_selector(const char *cmd, int argc, char **argv,
			       int idx, struct fyai_turn_selector_args *ts)
{
	static const char *const opts[] = {
		"--first", "--last", "--range", NULL,
	};
	int opti, start_idx, rc;
	unsigned int i;
	const char *arg, *val;

	start_idx = idx;
	arg = argv[idx];

	opti = str_in_set(arg, opts);
	if (opti < 0)	/* not any of ours */
		return 0;

	val = idx + 1 < argc ? argv[idx + 1] : NULL;

	/* all of our options need an argument */
	if (!val) {
		fprintf(stderr, "%s: option %s needs argument\n", cmd, arg);
		return -1;
	}

	/* only one allowed */
	if (ts->type != FYAITST_ALL) {
		fprintf(stderr, "%s: use only one of ", cmd);
		for (i = 0; i < ARRAY_SIZE(opts) - 1; i++)
			fprintf(stderr, "%s%s", i > 0 ? "/" : "", opts[i]);
		fprintf(stderr, "\n");
		return -1;
	}

	switch (opti) {
	case 0:	/* first */
		ts->type = FYAITST_FIRST;
		ts->first = atoi(val);
		idx++;
		break;
	case 1:	/* last */
		ts->type = FYAITST_LAST;
		ts->last = atoi(val);
		idx++;
		break;
	case 2:	/* range */
		rc = sscanf(val, "%zu,%zu", &ts->range_lo, &ts->range_hi);
		if (rc != 2) {
			fprintf(stderr, "%s: --range needs x,y\n", cmd);
			return -1;
		}
		ts->type = FYAITST_RANGE;
		idx++;
		break;

	default:
		/* impossible */
		break;
	}

	/* return the number we chomped */
	return idx - start_idx;
}

static int configure_dump(int argc, char **argv, struct fyai_cfg *cfg)
{
	static const char *const targets[] = {
		"state", "providers", "anchors", NULL,
	};
	struct fy_generic_builder *gb = cfg->gb;
	struct fyai_dump_args *args = &cfg->cmd.args.dump;
	const char *target;
	const char *a;
	int i, parsed, idx;

	(void)gb;
	target = NULL;
	args->turn_sel.type = FYAITST_ALL;
	for (i = 1; i < argc; i++) {
		parsed = parse_turn_selector("dump", argc, argv, i, &args->turn_sel);
		if (parsed < 0)
			return -1;
		if (parsed) {
			i += parsed;
			continue;
		}

		a = argv[i];
		if (*a == '-') {
			if (!strcmp(a, "--decorate")) {
				args->decorate = true;
				continue;
			}
			fprintf(stderr, "dump: unknown option '%s'\n", a);
			return -1;
		}

		target = a;
		break;
	}

	/* default to state */
	if (!target)
		target = "state";

	idx = str_in_set(target, targets);
	if (idx < 0) {
		fprintf(stderr, "dump: unknown target '%s' "
			"(state|anchors|providers)\n", target);
		return -1;
	}
	switch (idx) {
	case 0:	/* state */
		args->state = true;
		break;
	case 1:	/* providers */
		args->provider_stream = true;
		break;
	case 2:	/* anchors */
		args->anchors = true;
		break;
	default:
		break;
	}

	return 0;
}

static int configure_history(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fy_generic_builder *gb = cfg->gb;
	struct fyai_display_args *args = &cfg->cmd.args.display;
	const char *cmd = argv[0];
	const char *a;
	int i, parsed;

	(void)gb;
	args->turn_sel.type = FYAITST_ALL;
	for (i = 1; i < argc; i++) {
		parsed = parse_turn_selector(cmd, argc, argv, i, &args->turn_sel);
		if (parsed < 0)
			return -1;
		if (parsed) {
			i += parsed;
			continue;
		}

		a = argv[i];
		if (!strcmp(a, "--raw")) {
			args->raw = true;
		} else {
			fprintf(stderr, "%s: unknown option '%s'\n", cmd, a);
			return -1;
		}
	}

	return 0;
}

static int configure_stats(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_stats_args *args = &cfg->cmd.args.stats;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json"))
			args->format = FYAIOF_JSON;
		else if (!strcmp(argv[i], "--yaml"))
			args->format = FYAIOF_YAML;
		else if (!strcmp(argv[i], "--raw"))
			args->format = FYAIOF_RAW;
		else {
			fprintf(stderr, "stats: unknown option '%s'\n", argv[i]);
			return -1;
		}
	}
	return 0;
}

static int configure_gc(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_gc_args *args = &cfg->cmd.args.gc;
	char *endp;
	long v;
	int i;

	args->keep_reflogs = -1;	/* keep the whole ref log */

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--keep-reflogs") && i + 1 < argc) {
			errno = 0;
			v = strtol(argv[++i], &endp, 10);
			if (errno || *endp || v < 1) {
				fprintf(stderr,
					"gc: --keep-reflogs needs a count >= 1\n");
				return -1;
			}
			args->keep_reflogs = (int)v;
			continue;
		}
		fprintf(stderr, "gc: unknown option '%s'\n", argv[i]);
		return -1;
	}
	return 0;
}

static int configure_tool(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_tool_args *args = &cfg->cmd.args.tool;
	char *joined;

	args->name = NULL;
	args->args_json = NULL;
	if (argc < 2) {
		fprintf(stderr, "tool: usage: fyai tool <name> [json-args]\n");
		return -1;
	}
	args->name = fy_gb_intern_string(cfg->gb, argv[1]);
	/* Remaining args, if any, are the JSON object (joined); else stdin. */
	if (argc > 2) {
		joined = join_args(argc - 2, argv + 2);
		if (!joined)
			return -1;
		args->args_json = fy_gb_intern_string(cfg->gb, joined);
		free(joined);
	}
	return 0;
}

static int configure_init(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_init_args *args = &cfg->cmd.args.init;
	char *dir = NULL;
	const char *src_cfg = NULL;
	char *cwd = NULL, *real = NULL;
	bool cfg_explicit;
	int i;

	cwd = NULL;
	dir = NULL;
	src_cfg = "config.yaml";
	cfg_explicit = false;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f"))
			args->force = true;
		else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
			src_cfg = argv[++i];
			cfg_explicit = true;
		} else if (argv[i][0] != '-') {
			src_cfg = argv[i];
			cfg_explicit = true;
		} else {
			fprintf(stderr, "init: unknown option '%s'\n", argv[i]);
			goto err_out;
		}
	}
	if (!dir) {
		cwd = getcwd(NULL, 0);
		if (!cwd) {
			fprintf(stderr, "init: unable to get current directory\n");
			goto err_out;
		}
		dir = cwd;
	}

	real = realpath(dir, NULL);

	if (!real) {
		fprintf(stderr, "init: realpath of directory %s not found\n", dir);
		goto err_out;
	}

	free(cwd);
	cwd = NULL;

	args->dir = fy_gb_intern_string(cfg->gb, real);
	free(real);
	real = NULL;

	if (!is_writable_directory(args->dir)) {
		fprintf(stderr, "init: bad directory %s\n", args->dir);
		goto err_out;
	}

	/* the default config.yaml is opportunistic; an explicit one must exist */
	if (!cfg_explicit && access(src_cfg, R_OK)) {
		args->config = NULL;
		return 0;
	}

	real = realpath(src_cfg, NULL);
	if (!real) {
		fprintf(stderr, "init: realpath of config %s not found\n", src_cfg);
		goto err_out;
	}
	free(real);
	real = NULL;

	args->config = fy_gb_intern_string(cfg->gb, src_cfg);

	if (!is_readable_file(args->config)) {
		fprintf(stderr, "init: bad config %s\n", args->config);
		goto err_out;
	}

	return 0;

err_out:
	free(real);
	free(cwd);
	return -1;
}

static int configure_list(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_list_args *args = &cfg->cmd.args.list;
	static const char * const types[] = {
		[FYAILT_PROVIDERS] = "providers",
		[FYAILT_MODELS] = "models",
		[FYAILT_TURNS] = "turns",
		[FYAILT_EXCHANGES] = "exchanges",
		[FYAILT_REFLOG] = "reflog",
		NULL
	};
	const char *what;
	bool have_target;
	int i;
	int idx;

	what = "providers";
	have_target = false;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--raw")) {
			args->format = FYAIOF_RAW;
			continue;
		}
		if (!strcmp(argv[i], "--json")) {
			args->format = FYAIOF_JSON;
			continue;
		}
		if (!strcmp(argv[i], "--yaml")) {
			args->format = FYAIOF_YAML;
			continue;
		}
		if (!strcmp(argv[i], "--markdown")) {
			args->format = FYAIOF_MARKDOWN;
			continue;
		}
		if (!strcmp(argv[i], "--full")) {
			args->full = true;
			continue;
		}
		if (!strcmp(argv[i], "--brief")) {
			args->full = false;
			continue;
		}
		if (argv[i][0] == '-') {
			fprintf(stderr, "list: unknown option '%s'\n", argv[i]);
			return -1;
		}
		if (have_target) {
			fprintf(stderr, "list: too many targets\n");
			return -1;
		}
		what = argv[i];
		have_target = true;
	}

	idx = str_in_set(what, types);
	if (idx < 0) {	/* not any of ours */
		fprintf(stderr, "list: unknown target '%s' (providers|models|turns|exchanges|reflog)\n", what);
		return -1;
	}
	args->type = (enum fyai_list_type)idx;
	return 0;
}

/*
 * `list` renders through the one generic-to-Markdown renderer: the data is a
 * sequence of mappings, so the renderopts only carry the column selection and
 * the few headers whose humanized key is not what we want.
 */
static fy_generic list_renderopts(struct fy_generic_builder *gb,
				  struct fyai_list_args *args)
{
	fy_generic api = fy_mapping(gb, "api", fy_mapping(gb, "name", "API"));

	switch (args->type) {
	case FYAILT_TURNS:
		return fy_mapping(gb,
			"empty", "no turns",
			"keys", fy_sequence(gb, "index", "role", "provider", "api",
					    "tokens"),
			"columns", api);
	case FYAILT_EXCHANGES:
		return fy_mapping(gb,
			"empty", "no exchanges",
			"keys", fy_sequence(gb, "index", "provider", "api",
					    "tokens"),
			"columns", api);
	case FYAILT_REFLOG:
		return fy_mapping(gb,
			"empty", "no ref log",
			"keys", fy_sequence(gb, "index", "ref", "model", "kind"));
	case FYAILT_MODELS:
		return fy_mapping(gb,
			"empty", "no models",
			"keys", args->full ?
				fy_sequence(gb, "name", "display_name", "providers",
					    "context_window", "max_output_tokens",
					    "open_source", "modalities",
					    "capabilities") :
				fy_sequence(gb, "name", "providers",
					    "context_window", "max_output_tokens",
					    "open_source"),
			"columns", fy_mapping(gb,
				"name", fy_mapping(gb, "name", "Model"),
				"context_window", fy_mapping(gb, "name", "Context"),
				"max_output_tokens", fy_mapping(gb, "name", "Max Output"),
				"open_source", fy_mapping(gb, "name", "Open",
							  "align", "left"),
				"display_name", fy_mapping(gb, "name", "Display")));
	case FYAILT_PROVIDERS:
	default:
		return fy_mapping(gb,
			"empty", "no providers configured",
			"keys", fy_sequence(gb, "name", "models"),
			"columns", fy_mapping(gb,
				"name", fy_mapping(gb, "name", "Provider")));
	}
}

/* The renderopts are display-only: build them in the transient builder so
 * they never reach the durable arena. */
static int list_emit_markdown(struct fyai_ctx *ctx, fy_generic data)
{
	struct fyai_list_args *args = &ctx->cfg->cmd.args.list;
	fy_generic opts;

	assert(ctx->transient_gb);
	opts = list_renderopts(ctx->transient_gb, args);
	if (args->format == FYAIOF_RAW)
		opts = fy_assoc(ctx->transient_gb, opts, "raw", true);
	return fyai_generic_to_markdown(ctx, opts, data);
}

static int list_emit_generic(fy_generic data, bool json)
{
	enum fy_op_emit_flags flags;

	flags = FYOPEF_DISABLE_DIRECTORY | FYOPEF_OUTPUT_TYPE_STDOUT |
		FYOPEF_WIDTH_INF;
	if (json)
		flags |= FYOPEF_MODE_JSON | FYOPEF_STYLE_COMPACT;
	else
		flags |= FYOPEF_MODE_YAML_1_2 | FYOPEF_STYLE_PRETTY;

	(void)fy_emit(data, flags, NULL);
	putchar('\n');
	return 0;
}

static bool list_provider_is_active(struct fyai_cfg *cfg, const char *name)
{
	return cfg->provider && name && !strcmp(cfg->provider, name);
}

static bool list_model_is_active(struct fyai_cfg *cfg, fy_generic provider,
				 const char *model)
{
	fy_generic offers, o;

	if (!cfg->model || !model)
		return false;
	if (!strcmp(cfg->model, model))
		return true;
	if (!list_provider_is_active(cfg, fy_get(provider, "name", "")))
		return false;
	offers = fy_get(provider, "models");
	fy_foreach(o, offers) {
		if (!fy_equal(fy_get(o, "canonical_id"), model))
			continue;
		return fy_equal(fy_get(o, "provider_model_id"), cfg->model);
	}
	return false;
}

/* Provider rows sourced from the effective catalogue. */
static fy_generic list_catalog_providers_data(struct fyai_cfg *cfg,
					      struct fy_generic_builder *gb,
					      bool full)
{
	fy_generic cat, providers, p, offers, o, out, item, models;
	const char *model, *name;

	(void)full;
	out = fy_seq_empty;
	cat = fyai_catalog_effective(cfg->catalog, gb);
	providers = fy_get(cat, "providers");

	fy_foreach(p, providers) {
		name = fy_get(p, "name", "");
		offers = fy_get(p, "models");
		models = fy_seq_empty;
		fy_foreach(o, offers) {
			model = fy_get(o, "canonical_id", "");
			if (list_provider_is_active(cfg, name) && cfg->model &&
			    (fy_equal(fy_get(o, "canonical_id"), cfg->model) ||
			     fy_equal(fy_get(o, "provider_model_id"), cfg->model)))
				model = fy_sprintfa("**%s**", model);
			models = fy_append(gb, models, fy_value(gb, model));
		}

		item = fy_null_filtered_mapping(gb,
			"name", fy_value(gb, list_provider_is_active(cfg, name) ?
					 fy_sprintfa("**%s**", name) : name),
			"models", models);

		out = fy_append(gb, out, item);
	}
	return out;
}

static fy_generic list_catalog_models_data(struct fyai_cfg *cfg,
					   struct fy_generic_builder *gb,
					   bool full)
{
	fy_generic cat, models, m, providers, p, offers, o, out, names;
	const char *model;
	bool active;

	cat = fyai_catalog_effective(cfg->catalog, gb);
	models = fy_get(cat, "models");
	providers = fy_get(cat, "providers");

	out = fy_seq_empty;
	fy_foreach(m, models) {
		model = fy_get(m, "name", "");
		active = false;
		names = fy_seq_empty;
		fy_foreach(p, providers) {
			offers = fy_get(p, "models");
			fy_foreach(o, offers) {
				if (!fy_equal(fy_get(o, "canonical_id"), model))
					continue;
				names = fy_append(gb, names,
					fy_value(gb,
						list_provider_is_active(cfg,
							fy_get(p, "name", "")) ?
						fy_sprintfa("**%s**",
							fy_get(p, "name", "")) :
						fy_get(p, "name", "")));
				active |= list_model_is_active(cfg, p, model);
				break;
			}
		}
		out = fy_append(gb, out,
			fy_null_filtered_mapping(gb,
				"name", fy_value(gb, active ?
						 fy_sprintfa("**%s**", model) :
						 model),
				"providers", names,
				"context_window", fy_get(m, "context_window", 0LL),
				"max_output_tokens", fy_get(m, "max_output_tokens", 0LL),
				"open_source", fy_get(m, "open_source", false),
				"display_name", full ? fy_get(m, "display_name", fy_null) : fy_null,
				"modalities", full ? fy_get(m, "modalities", fy_seq_empty) : fy_null,
				"capabilities", full ? fy_get(m, "capabilities", fy_seq_empty) : fy_null));
	}
	return out;
}

int fyai_execute_list(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_list_args *args = &cfg->cmd.args.list;
	struct fy_generic_builder_cfg gcfg;
	struct fy_generic_builder *gb;
	fy_generic data;
	int rc;

	memset(&gcfg, 0, sizeof(gcfg));
	gcfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED;
	gb = fy_generic_builder_create(&gcfg);
	if (!gb)
		return -1;

	switch (args->type) {
	case FYAILT_PROVIDERS:
	default:
		data = list_catalog_providers_data(cfg, gb, args->full);
		break;
	case FYAILT_MODELS:
		data = cfg->auth_mode == FYAI_AUTH_CHATGPT ?
			fyai_auth_models(ctx, gb, args->full) :
			list_catalog_models_data(cfg, gb, args->full);
		break;
	case FYAILT_TURNS:
		data = fyai_list_turns_data(ctx, gb);
		break;
	case FYAILT_EXCHANGES:
		data = fyai_list_exchanges_data(ctx, gb);
		break;
	case FYAILT_REFLOG:
		data = fyai_list_reflog_data(ctx, gb);
		break;
	}
	if (fy_generic_is_invalid(data)) {
		fy_generic_builder_destroy(gb);
		return -1;
	}

	switch (args->format) {
	case FYAIOF_JSON:
		rc = list_emit_generic(data, true);
		break;
	case FYAIOF_YAML:
		rc = list_emit_generic(data, false);
		break;
	case FYAIOF_RAW:
	case FYAIOF_MARKDOWN:
	default:
		rc = list_emit_markdown(ctx, data);
		break;
	}
	fy_generic_builder_destroy(gb);
	return rc;
}

/*
 * `config describe [path]` - render a markdown table (name | type |
 * constraint | description) for a config.schema.yaml node, resolved by a
 * slash path into `properties` (transparently descending oneOf/anyOf
 * branches too, e.g. "sandbox/enabled"). An object node lists its
 * properties, one row each; a leaf node prints a single-row table for
 * itself. No path describes the whole document.
 */
/*
 * A schema `default` (system_prompt's, notably) can be arbitrarily long in
 * a live config; cap every cell so one oversized value can't blow up the
 * table.
 */
#define DESCRIBE_CELL_MAX 160

/* @node's `default`, stringified into @buf; empty when absent. */
static void schema_default_str(fy_generic node, char *buf, size_t bufsz)
{
	fy_generic v;

	v = fy_get(node, "default");
	if (fy_generic_is_invalid(v)) {
		buf[0] = '\0';
		return;
	}
	/* fy_convert()'s result has stack lifetime bound to this frame -
	 * copy it out with snprintf before returning, never cache the
	 * pointer itself. */
	snprintf(buf, bufsz, "%s", fy_cast(fy_convert(v, FYGT_STRING), ""));
}

static void schema_type_str(fy_generic node, char *buf, size_t bufsz)
{
	fy_generic type, v, branches;
	char sub[64];
	size_t off;
	int n;

	type = fy_get(node, "type");
	if (fy_generic_is_string(type)) {
		snprintf(buf, bufsz, "%s", fy_castp(&type, ""));
		return;
	}
	off = 0;
	if (fy_generic_is_sequence(type)) {
		fy_foreach(v, type) {
			n = snprintf(buf + off, off < bufsz ? bufsz - off : 0,
				    "%s%s", off ? "|" : "", fy_castp(&v, ""));
			if (n > 0)
				off += (size_t)n;
		}
		if (off)
			return;
	}
	branches = fy_get(node, "oneOf");
	if (fy_generic_is_invalid(branches))
		branches = fy_get(node, "anyOf");
	if (fy_generic_is_sequence(branches)) {
		off = 0;
		fy_foreach(v, branches) {
			schema_type_str(v, sub, sizeof(sub));
			n = snprintf(buf + off, off < bufsz ? bufsz - off : 0,
				    "%s%s", off ? " or " : "", sub);
			if (n > 0)
				off += (size_t)n;
		}
		if (off)
			return;
	}
	snprintf(buf, bufsz, "any");
}

static double schema_num(fy_generic v)
{
	if (fy_generic_is_int(v))
		return (double)fy_cast(v, 0LL);
	if (fy_generic_is_float(v))
		return fy_cast(v, 0.0);
	return 0.0;
}

static void schema_constraint_str(fy_generic node, bool required, char *buf,
				  size_t bufsz)
{
	fy_generic v, sub;
	char items[192], itype[64];
	size_t off, ioff;
	bool first, ifirst;
	int n;

	off = 0;
	first = true;

#define ADD(...) do { \
	n = snprintf(buf + off, off < bufsz ? bufsz - off : 0, "%s", \
		    first ? "" : "; "); \
	if (n > 0) \
		off += (size_t)n; \
	n = snprintf(buf + off, off < bufsz ? bufsz - off : 0, __VA_ARGS__); \
	if (n > 0) \
		off += (size_t)n; \
	first = false; \
} while (0)

	if (required)
		ADD("required");

	v = fy_get(node, "const");
	if (fy_generic_is_valid(v))
		ADD("const: %s", fy_castp(&v, ""));

	v = fy_get(node, "enum");
	if (fy_generic_is_sequence(v)) {
		ioff = 0;
		ifirst = true;
		fy_foreach(sub, v) {
			n = snprintf(items + ioff,
				    ioff < sizeof(items) ? sizeof(items) - ioff : 0,
				    "%s%s", ifirst ? "" : ", ", fy_castp(&sub, ""));
			if (n > 0)
				ioff += (size_t)n;
			ifirst = false;
		}
		ADD("one of: %s", items);
	}

	v = fy_get(node, "minimum");
	if (fy_generic_is_valid(v))
		ADD(">= %g", schema_num(v));
	v = fy_get(node, "maximum");
	if (fy_generic_is_valid(v))
		ADD("<= %g", schema_num(v));
	v = fy_get(node, "exclusiveMinimum");
	if (fy_generic_is_valid(v))
		ADD("> %g", schema_num(v));
	v = fy_get(node, "exclusiveMaximum");
	if (fy_generic_is_valid(v))
		ADD("< %g", schema_num(v));

	v = fy_get(node, "minLength");
	if (fy_generic_is_valid(v))
		ADD("minLength %g", schema_num(v));
	v = fy_get(node, "maxLength");
	if (fy_generic_is_valid(v))
		ADD("maxLength %g", schema_num(v));

	v = fy_get(node, "pattern");
	if (fy_generic_is_string(v))
		ADD("pattern: `%s`", fy_castp(&v, ""));

	v = fy_get(node, "minItems");
	if (fy_generic_is_valid(v))
		ADD("minItems %g", schema_num(v));
	v = fy_get(node, "maxItems");
	if (fy_generic_is_valid(v))
		ADD("maxItems %g", schema_num(v));

	v = fy_get(node, "items");
	if (fy_generic_is_mapping(v)) {
		schema_type_str(v, itype, sizeof(itype));
		ADD("items: %s", itype);
	}

#undef ADD
	if (first)
		snprintf(buf, bufsz, "-");
}

/* Descend into `properties`, transparently trying oneOf/anyOf branches. */
static fy_generic schema_child(fy_generic node, const char *key)
{
	fy_generic props, v, branches, b;

	props = fy_get(node, "properties");
	if (fy_generic_is_mapping(props)) {
		v = fy_get(props, key);
		if (fy_generic_is_valid(v))
			return v;
	}
	branches = fy_get(node, "oneOf");
	if (fy_generic_is_invalid(branches))
		branches = fy_get(node, "anyOf");
	fy_foreach(b, branches) {
		v = schema_child(b, key);
		if (fy_generic_is_valid(v))
			return v;
	}
	return fy_invalid;
}

/*
 * @node itself if it has `properties` directly; otherwise the first
 * oneOf/anyOf branch that does (recursively) - so describing a node like
 * `sandbox` (oneOf [boolean, object-with-properties]) still lists the
 * object branch's properties instead of falling back to a single leaf row.
 */
static fy_generic schema_object_node(fy_generic node)
{
	fy_generic props, branches, b, sub;

	props = fy_get(node, "properties");
	if (fy_generic_is_mapping(props))
		return node;
	branches = fy_get(node, "oneOf");
	if (fy_generic_is_invalid(branches))
		branches = fy_get(node, "anyOf");
	fy_foreach(b, branches) {
		sub = schema_object_node(b);
		if (fy_generic_is_valid(sub))
			return sub;
	}
	return fy_invalid;
}

static bool schema_key_required(fy_generic node, const char *key)
{
	fy_generic req, v;

	req = fy_get(node, "required");
	if (!fy_generic_is_sequence(req))
		return false;
	fy_foreach(v, req)
		if (!strcmp(fy_castp(&v, ""), key))
			return true;
	return false;
}

/* One row of the describe table. */
static fy_generic describe_row(struct fy_generic_builder *gb, const char *name,
			       fy_generic node, bool required, fy_generic desc)
{
	char type_buf[128], cons_buf[384];
	char def_buf[DESCRIBE_CELL_MAX + 8];

	schema_type_str(node, type_buf, sizeof(type_buf));
	schema_constraint_str(node, required, cons_buf, sizeof(cons_buf));
	schema_default_str(node, def_buf, sizeof(def_buf));
	return fy_mapping(gb,
		"name", name,
		"type", type_buf,
		"constraint", *cons_buf ? cons_buf : "-",
		"default", *def_buf ? def_buf : "-",
		"description", fy_generic_is_string(desc) ?
			fy_castp(&desc, "") : "");
}

static int config_describe(struct fyai_ctx *ctx, const char *path)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic schema, node, object_node, props, desc, key, sub;
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic rows;
	char seg[256];
	const char *p, *slash, *last;
	size_t seglen, i, n;

	assert(gb);
	schema = fyai_config_schema(cfg->gb);
	node = schema;
	last = "config";

	if (path && *path) {
		p = path;
		while (*p) {
			slash = strchr(p, '/');
			seglen = slash ? (size_t)(slash - p) : strlen(p);
			if (seglen >= sizeof(seg))
				seglen = sizeof(seg) - 1;
			memcpy(seg, p, seglen);
			seg[seglen] = '\0';
			node = schema_child(node, seg);
			if (fy_generic_is_invalid(node)) {
				fprintf(stderr,
					"config: describe: unknown property '%s'\n",
					path);
				return -1;
			}
			last = seg;
			p = slash ? slash + 1 : p + seglen;
		}
	}

	desc = fy_get(node, "description");
	rows = fy_seq_empty;

	/* Transparently prefer an object oneOf/anyOf branch (e.g. sandbox's
	 * object form) over treating the node as a leaf, so a node with no
	 * *direct* `properties` but a nested object branch still drills down
	 * into that branch's properties instead of describing itself. */
	object_node = schema_object_node(node);
	props = fy_generic_is_valid(object_node) ?
		fy_get(object_node, "properties") : fy_invalid;
	if (!fy_generic_is_mapping(props)) {
		rows = fy_append(gb, rows,
				 describe_row(gb, last, node, false, desc));
	} else {
		n = fy_generic_mapping_get_pair_count(props);
		for (i = 0; i < n; i++) {
			key = fy_generic_mapping_get_at_key(props, i);
			sub = fy_generic_mapping_get_at_value(props, i);
			rows = fy_append(gb, rows,
				describe_row(gb, fy_castp(&key, ""), sub,
					     schema_key_required(object_node,
						fy_castp(&key, "")),
					     fy_get(sub, "description")));
		}
	}

	return fyai_generic_to_markdown(ctx,
		fy_mapping(gb,
			"preamble", fy_sprintfa("## %s%s%s",
				(path && *path) ? path : "config",
				fy_generic_is_string(desc) ? "\n\n" : "",
				fy_generic_is_string(desc) ?
					fy_castp(&desc, "") : ""),
			"columns", fy_mapping(gb,
				"name", fy_mapping(gb, "name", "name"),
				"type", fy_mapping(gb, "name", "type"),
				"constraint", fy_mapping(gb, "name",
							 "constraint"),
				"default", fy_mapping(gb, "name",
						      "default"),
				"description", fy_mapping(gb, "name",
							  "description"))),
		rows);
}

static int configure_config(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_config_args *args = &cfg->cmd.args.config;
	static const char * const types[] = {
		[FYAICT_SHOW] = "show",
		[FYAICT_EFFECTIVE] = "effective",
		[FYAICT_GET] = "get",
		[FYAICT_SET] = "set",
		[FYAICT_DELETE] = "delete",
		[FYAICT_EDIT] = "edit",
		[FYAICT_IMPORT] = "import",
		[FYAICT_EXPORT] = "export",
		[FYAICT_VALIDATE] = "validate",
		[FYAICT_SCHEMA] = "schema",
		[FYAICT_DESCRIBE] = "describe",
		NULL
	};
	const char *what;
	int i, idx;

	i = 1;
	what = i < argc ? argv[i] : "show";

	idx = str_in_set(what, types);
	if (idx < 0) {	/* not any of ours */
		fprintf(stderr,
			"config: unknown type '%s' "
			"(show|effective|get|set|delete|edit|import|export|validate|schema|describe)\n",
			what);
		return -1;
	}
	args->type = (enum fyai_config_type)idx;
	i++;
	args->key = i < argc ? fy_gb_intern_string(cfg->gb, argv[i++]) : NULL;
	args->value = i < argc ? fy_gb_intern_string(cfg->gb, argv[i++]) : NULL;

	switch (args->type) {
	case FYAICT_SHOW:
	case FYAICT_EFFECTIVE:
		break;
	case FYAICT_GET:
		if (!args->key) {
			fprintf(stderr, "config: get: missing key\n");
			return -1;
		}
		break;
	case FYAICT_SET:
		if (!args->key) {
			fprintf(stderr, "config: set: missing key\n");
			return -1;
		}
		if (!args->value) {
			fprintf(stderr, "config: set: missing value\n");
			return -1;
		}
		break;
	case FYAICT_DELETE:
		if (!args->key) {
			fprintf(stderr, "config: delete: missing key\n");
			return -1;
		}
		break;
	case FYAICT_IMPORT:
		if (!args->key) {
			fprintf(stderr, "config: import: missing file\n");
			return -1;
		}
		break;
	case FYAICT_EXPORT:	/* optional output file in args->key */
	case FYAICT_EDIT:
	case FYAICT_VALIDATE:
	case FYAICT_SCHEMA:
	case FYAICT_DESCRIBE:	/* optional slash path in args->key */
		break;
	default:
		break;
	}
	return 0;
}

int fyai_execute_config(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_config_args *args = &cfg->cmd.args.config;
	int rc;

	switch (args->type) {
	case FYAICT_NOOP:
		/* Global --set/--get/--delete already ran in fyai_run. */
		break;
	case FYAICT_SHOW:
		rc = fyai_config_export(ctx, NULL);
		if (rc)
			return -1;
		break;
	case FYAICT_EFFECTIVE:
		rc = fyai_config_show(cfg);
		if (rc)
			return -1;
		break;
	case FYAICT_GET:
		rc = fyai_config_get(ctx, args->key);
		if (rc)
			return -1;
		break;
	case FYAICT_SET:
		rc = fyai_config_set(ctx, args->key, args->value);
		if (rc)
			return -1;
		break;
	case FYAICT_DELETE:
		rc = fyai_config_delete(ctx, args->key);
		if (rc)
			return -1;
		break;
	case FYAICT_EDIT:
		rc = fyai_config_edit(ctx);
		if (rc)
			return -1;
		break;
	case FYAICT_IMPORT:
		rc = fyai_config_import(ctx, args->key);
		if (rc)
			return -1;
		break;
	case FYAICT_EXPORT:
		rc = fyai_config_export(ctx, args->key);
		if (rc)
			return -1;
		break;
	case FYAICT_VALIDATE:
		rc = fyai_config_validate_schema(cfg, ctx->arena_config,
						 "config");
		if (rc)
			return -1;
		printf("config: valid\n");
		break;
	case FYAICT_SCHEMA:
		emit_generic_to_stdout(NULL, fyai_config_schema(cfg->gb), true);
		break;
	case FYAICT_DESCRIBE:
		rc = config_describe(ctx, args->key);
		if (rc)
			return -1;
		break;
	}
	return 0;
}

static int configure_catalog(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_catalog_args *args = &cfg->cmd.args.catalog;
	static const char * const types[] = {
		[FYAICAT_SHOW] = "show",
		[FYAICAT_LIST] = "list",
		[FYAICAT_TOOLS] = "tools",
		[FYAICAT_IMPORT] = "import",
		[FYAICAT_EXPORT] = "export",
		NULL
	};
	const char *what;
	int i, idx;

	i = 1;
	what = i < argc ? argv[i] : "show";

	idx = str_in_set(what, types);
	if (idx < 0) {
		fprintf(stderr,
			"catalog: unknown type '%s' (show|list|tools|import|export)\n",
			what);
		return -1;
	}
	args->type = (enum fyai_catalog_type)idx;
	i++;
	args->arg = NULL;
	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--full")) {
			args->full = true;
			continue;
		}
		if (!strcmp(argv[i], "--brief")) {
			args->full = false;
			continue;
		}
		if (argv[i][0] == '-') {
			fprintf(stderr, "catalog: unknown option '%s'\n", argv[i]);
			return -1;
		}
		if (args->arg) {
			fprintf(stderr, "catalog: too many arguments\n");
			return -1;
		}
		args->arg = fy_gb_intern_string(cfg->gb, argv[i]);
	}

	if (args->type == FYAICAT_IMPORT && !args->arg) {
		fprintf(stderr, "catalog: import: missing file\n");
		return -1;
	}
	if (args->type == FYAICAT_LIST && args->arg &&
	    strcmp(args->arg, "models") && strcmp(args->arg, "providers")) {
		fprintf(stderr, "catalog: list: models or providers\n");
		return -1;
	}
	return 0;
}

static int execute_catalog(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_catalog_args *args = &cfg->cmd.args.catalog;

	switch (args->type) {
	case FYAICAT_SHOW:
		return fyai_catalog_show(ctx);
	case FYAICAT_LIST:
		return fyai_catalog_list(ctx, args->arg);
	case FYAICAT_TOOLS:
		return fyai_catalog_tools(ctx, args->arg, args->full);
	case FYAICAT_IMPORT:
		return fyai_catalog_import(ctx, args->arg);
	case FYAICAT_EXPORT:
		return fyai_catalog_export(ctx, args->arg);
	}
	return -1;
}

static int configure_clear(int argc, char **argv, struct fyai_cfg *cfg)
{
	(void)cfg;
	if (argc > 1) {
		fprintf(stderr, "clear: unexpected argument '%s'\n", argv[1]);
		return -1;
	}
	return 0;
}

static int execute_clear(struct fyai_ctx *ctx)
{
	return fyai_session_clear(ctx);
}

static int configure_compact(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_compact_args *args = &cfg->cmd.args.compact;
	char *hint;

	args->hint = NULL;
	if (argc > 1) {
		hint = join_args(argc - 1, argv + 1);
		if (!hint)
			return -1;
		args->hint = fy_gb_intern_string(cfg->gb, hint);
		free(hint);
	}
	return 0;
}

static int execute_compact(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_compact_args *args = &cfg->cmd.args.compact;
	int rc;

	rc = fyai_setup_transient_builder(ctx);
	if (rc)
		return -1;
	rc = fyai_session_compact(ctx, args->hint);
	fyai_cleanup_transient_builder(ctx);
	return rc;
}

static int configure_context(int argc, char **argv, struct fyai_cfg *cfg)
{
	(void)cfg;
	if (argc > 1) {
		fprintf(stderr, "context: unexpected argument '%s'\n", argv[1]);
		return -1;
	}
	return 0;
}

static int execute_context(struct fyai_ctx *ctx)
{
	return fyai_session_context(ctx);
}

static int configure_api(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_api_args *args = &cfg->cmd.args.api;

	args->mode = NULL;
	if (argc > 2) {
		fprintf(stderr, "api: unexpected argument '%s'\n", argv[2]);
		return -1;
	}
	if (argc > 1)
		args->mode = fy_gb_intern_string(cfg->gb, argv[1]);
	return 0;
}

static int execute_api(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_api_args *args = &cfg->cmd.args.api;

	/* Validate and resolve the switch (prints the outcome); with an
	 * argument also persist the arena `api` key so later runs keep it. */
	if (fyai_session_api(ctx, args->mode))
		return -1;
	if (!args->mode || !*args->mode)
		return 0;
	return fyai_config_set(ctx, "api",
			       fyai_api_to_string(cfg->api_mode));
}

static int configure_log(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_log_args *args = &cfg->cmd.args.log;
	char *arg;

	args->arg = NULL;
	if (argc > 1) {
		arg = join_args(argc - 1, argv + 1);
		if (!arg)
			return -1;
		args->arg = fy_gb_intern_string(cfg->gb, arg);
		free(arg);
	}
	return 0;
}

static int execute_log(struct fyai_ctx *ctx)
{
	return fyai_log_control(ctx, ctx->cfg->cmd.args.log.arg);
}

static int configure_sandbox(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_config_args *args = &cfg->cmd.args.config;
	const char *what;

	what = argc > 1 ? argv[1] : "show";
	if (!strcmp(what, "show") || !strcmp(what, "get")) {
		args->type = FYAICT_GET;
		args->key = "sandbox";
		args->value = NULL;
	} else if (!strcmp(what, "enable") || !strcmp(what, "on")) {
		args->type = FYAICT_SET;
		args->key = "sandbox";
		args->value = "{ enabled: true, deny: [secrets, ~/.ssh], network: { ports: [443] } }";
	} else if (!strcmp(what, "disable") || !strcmp(what, "off")) {
		args->type = FYAICT_SET;
		args->key = "sandbox";
		args->value = "false";
	} else if (!strcmp(what, "edit")) {
		args->type = FYAICT_EDIT;
		args->key = NULL;
		args->value = NULL;
	} else {
		fprintf(stderr,
			"sandbox: use show|on|off|edit\n");
		return -1;
	}
	if (argc > 2) {
		fprintf(stderr, "sandbox: unexpected argument '%s'\n", argv[2]);
		return -1;
	}
	return 0;
}

static int configure_auth(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_auth_args *args = &cfg->cmd.args.auth;
	const char *what;
	int i = 1;

	args->provider = "openai";
	if (i < argc && fy_not_equal(argv[i], "status") &&
	    fy_not_equal(argv[i], "info") && fy_not_equal(argv[i], "usage") &&
	    fy_not_equal(argv[i], "login") && fy_not_equal(argv[i], "logout"))
		args->provider = fy_gb_intern_string(cfg->gb, argv[i++]);
	what = i < argc ? argv[i++] : "status";

	if (fy_equal(what, "status"))
		args->command = FYAI_AUTH_STATUS;
	else if (fy_equal(what, "info"))
		args->command = FYAI_AUTH_INFO;
	else if (fy_equal(what, "usage"))
		args->command = FYAI_AUTH_USAGE;
	else if (fy_equal(what, "login"))
		args->command = FYAI_AUTH_LOGIN;
	else if (fy_equal(what, "logout"))
		args->command = FYAI_AUTH_LOGOUT;
	else {
		fprintf(stderr, "auth: unknown command '%s' (status|info|usage|login|logout)\n",
			what);
		return -1;
	}

	for (; i < argc; i++) {
		if (args->command == FYAI_AUTH_LOGIN &&
		    !strcmp(argv[i], "--device-code"))
			args->device_code = true;
		else if (args->command == FYAI_AUTH_LOGIN &&
			 !strcmp(argv[i], "--no-browser"))
			args->no_browser = true;
		else if (args->command == FYAI_AUTH_LOGIN &&
			 (!strcmp(argv[i], "--manual") ||
			  !strcmp(argv[i], "--paste")))
			args->manual = true;
		else if ((args->command == FYAI_AUTH_STATUS ||
			  args->command == FYAI_AUTH_INFO ||
			  args->command == FYAI_AUTH_USAGE) &&
			 !strcmp(argv[i], "--json"))
			args->json = true;
		else {
			fprintf(stderr, "auth: unexpected argument '%s'\n", argv[i]);
			return -1;
		}
	}
	return 0;
}

static int configure_secret(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_secret_args *a = &cfg->cmd.args.secret;
	int i = 1;

	if (i >= argc || fy_equal(argv[i], "status")) {
		a->command = FYAI_SECRET_STATUS;
		if (i < argc)
			i++;
	} else if (fy_equal(argv[i], "set")) {
		a->command = FYAI_SECRET_SET;
		i++;
	} else if (fy_equal(argv[i], "delete")) {
		a->command = FYAI_SECRET_DELETE;
		i++;
	} else {
		fprintf(stderr, "secret: use status [name]|set <name>|delete <name>\n");
		return -1;
	}
	if (i < argc && strcmp(argv[i], "--stdin"))
		a->name = fy_gb_intern_string(cfg->gb, argv[i++]);
	if (i < argc && a->command == FYAI_SECRET_SET && !strcmp(argv[i], "--stdin")) {
		a->stdin_value = true;
		i++;
	}
	if ((a->command == FYAI_SECRET_SET || a->command == FYAI_SECRET_DELETE) &&
	    (!a->name || !*a->name)) {
		fprintf(stderr, "secret: a logical name is required\n");
		return -1;
	}
	if (i < argc) {
		fprintf(stderr, "secret: unexpected argument '%s'\n", argv[i]);
		return -1;
	}
	return 0;
}

static int configure_help(int argc, char **argv, struct fyai_cfg *cfg);
static int execute_help(struct fyai_ctx *ctx);

static int configure_prompt(int argc, char **argv, struct fyai_cfg *cfg)
{
	(void)argc;
	(void)argv;
	(void)cfg;

	return 0;
}

static const struct fyai_verb fyai_verbs[FYAI_VERB_COUNT] = {
	[FYAIVID_PROMPT] = {
		.id	   = FYAIVID_PROMPT,
		.name	   = "prompt",
		.configure = configure_prompt,
		.execute   = fyai_prompt,
		.synopsis  = "prompt",
		.help      = "The default prompt mode\n",
		.flags	   = FYAIVF_INTERACTIVE | FYAIVF_NEEDS_API_KEYS,
		.default_args.prompt = {
		},
	},
	[FYAIVID_INIT] = {
		.id	   = FYAIVID_INIT,
		.name	   = "init",
		.configure = configure_init,
		.execute   = fyai_init_storage,
		.synopsis  = "init [--config <file>] [--force] [path]",
		.help      = "Create ./.fyai/arena/ and publish its container root.\n"
			     "A config file (positional or --config; default config.yaml when\n"
			     "present) is ingested into the arena; --force re-ingests over an\n"
			     "existing config.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_STORAGE | FYAIVF_NO_REQUESTS,
		.default_args.init = {
			.force = false,
			.config = "config.yaml",
		},
	},
	[FYAIVID_DUMP] = {
		.id	   = FYAIVID_DUMP,
		.name	   = "dump",
		.configure = configure_dump,
		.execute   = fyai_dump_view,
	  	.synopsis  = "dump [state|anchors|providers] [--first n|--last n|--range x,y] [--decorate]",
		.help      = "Emit conversation state as YAML to stdout (default: state).\n"
			     "  state      canonical, provider-agnostic conversation (replayed)\n"
			     "  anchors    full turn graph with anchors (streams + usage)\n"
			     "  providers  per-turn provider-specific wire streams\n"
			     "--first/--last/--range select a turn window (0-based, range\n"
			     "inclusive; state/providers only). --decorate adds a top comment\n"
			     "to each entry with the turn number, role, and provider.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.dump = {
			.decorate = false,
			.state = true,
			.anchors = false,
			.provider_stream = false,
			.turn_sel.type = FYAITST_ALL,
		},
	},
	[FYAIVID_DISPLAY] = {
		.id	   = FYAIVID_DISPLAY,
		.name	   = "display",
		.configure = configure_history,
		.execute   = fyai_display_view,
		.synopsis  = "display [--raw] [--first n|--last n|--range x,y]",
		.help	   = "Alias for `history`.\n",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.display = {
			.raw = false,
			.turn_sel.type = FYAITST_ALL,
		},
	},
	[FYAIVID_HISTORY] = {
		.id	   = FYAIVID_HISTORY,
		.name	   = "history",
		.configure = configure_history,
		.execute   = fyai_display_view,
		.synopsis  = "history [--raw] [--first n|--last n|--range x,y]",
		.help	   = "Render the canonical conversation history in a human-digestible form\n"
			     "(markdown via libfymd4c): exchanges separated by a rule, assistant tool\n"
			     "calls as the invoked command, and tool results collapsed to a\n"
			     "one-line size summary rather than reproduced. Unlike dump it is\n"
			     "not a faithful serialization. --raw prints the generated markdown.\n"
			     "--first/--last/--range select exchanges\n"
			     "(0-based, range inclusive).",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.display = {
			.raw = false,
			.turn_sel.type = FYAITST_ALL,
		},
	},
	[FYAIVID_STATS] = {
		.id	   = FYAIVID_STATS,
		.name	   = "stats",
		.configure = configure_stats,
		.execute   = fyai_show_stats,
		.synopsis  = "stats [--raw|--json|--yaml]",
		.help	   = "Report cumulative token/cost usage summed over the persisted turn\n"
			     "chain: calls, input, cached, cache_write, output, reasoning, total,\n"
			     "cost. Default output is rendered markdown; --raw prints generated\n"
			     "markdown, --json emits JSON, and --yaml emits YAML. (The --stats global option\n"
			     "instead prints only the current run's usage to stderr.)",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.stats = {
			.format = FYAIOF_MARKDOWN,
		},
	},
	[FYAIVID_CONFIG] = {
		.id	   = FYAIVID_CONFIG,
		.name	   = "config",
		.configure = configure_config,
		.execute   = fyai_execute_config,
		.synopsis  = "config [show | effective | get <key> | set <key> <value> | delete <key> | import <file> | export [file] | edit | validate | schema | describe [path]]",
		.help	   = "Inspect or edit the arena-resident configuration (default: show).\n"
			     "  show              print the stored arena config\n"
			     "  effective         print the merged effective config (no secrets)\n"
			     "  get <key>         print a key as one-line flow (slash path: display/color)\n"
			     "  set <key> <value> set a key (value parsed as a YAML flow document)\n"
			     "  delete <key>      remove a key (slash path)\n"
			     "  import <file>     ingest a YAML config file into the arena\n"
			     "  export [file]     emit the arena config as YAML (stdout by default)\n"
			     "  edit              edit the arena config with $VISUAL/$EDITOR\n"
			     "  validate          check the stored config against the JSON Schema\n"
			     "  schema            print the config document JSON Schema (YAML)\n"
			     "  describe [path]   markdown table of a schema node (slash path,\n"
			     "                    e.g. display/color); whole document if omitted",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS |
			     FYAIVF_NEEDS_TRANSIENT_BUILDER,
		.default_args.config = {
		},
	},
	[FYAIVID_LIST] = {
		.id	   = FYAIVID_LIST,
		.name	   = "list",
		.configure = configure_list,
		.execute   = fyai_execute_list,
		.synopsis  = "list [--raw|--json|--yaml] [providers|models|turns|exchanges|reflog]",
		.help      = "List configured items as rendered markdown (default: providers).\n"
			     "  providers  configured providers (api, model; * = active)\n"
			     "  models     catalogue models (context and max output)\n"
			     "  turns      one compact row per internal storage turn\n"
			     "  exchanges  one compact row per user exchange\n"
			     "  reflog     one compact row per arena root update\n"
			     "  --raw      print generated markdown without rendering\n"
			     "  --json     emit JSON\n"
			     "  --yaml     emit YAML\n"
			     "  --full     include per-item detail where available\n"
			     "  --brief    summary output (default)",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS |
			     FYAIVF_NEEDS_TRANSIENT_BUILDER,
		.default_args.list = {
		},
	},
	[FYAIVID_CATALOG] = {
		.id	   = FYAIVID_CATALOG,
		.name	   = "catalog",
		.configure = configure_catalog,
		.execute   = execute_catalog,
		.synopsis  = "catalog [show | list [models|providers] | tools [agent] | import <file> | export [file]]",
		.help	   = "Inspect, ingest or export the provider/model catalogue (default: show).\n"
			     "  show              emit the effective catalogue as YAML\n"
			     "  list [what]       tabulate models and/or providers\n"
			     "  import <file>     ingest a scrape-providers YAML into the arena\n"
			     "  export [file]     write the effective catalogue as YAML (stdout if omitted)\n"
			     "  tools [agent]     list agent tools (--brief|--full)\n"
			     "Without an arena catalogue the build-time embedded snapshot is used.\n"
			     "The catalogue is view/import/export only: there is no in-place edit,\n"
			     "unlike config.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.catalog = {
		},
	},
	[FYAIVID_CLEAR] = {
		.id	   = FYAIVID_CLEAR,
		.name	   = "clear",
		.configure = configure_clear,
		.execute   = execute_clear,
		.synopsis  = "clear",
		.help	   = "Reset the conversation: publish a null head. Prior turns stay in\n"
			     "the immutable arena (unreachable until gc). Same backend as the\n"
			     "interactive /clear.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.clear = {
		},
	},
	[FYAIVID_COMPACT] = {
		.id	   = FYAIVID_COMPACT,
		.name	   = "compact",
		.configure = configure_compact,
		.execute   = execute_compact,
		.synopsis  = "compact [hint]",
		.help	   = "Summarize the conversation with one model call and restart the\n"
			     "chain from the summary (old head kept as compacted_from metadata\n"
			     "for provenance). An optional hint focuses the summary. Needs an\n"
			     "API key. Same backend as the interactive /compact.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NEEDS_API_KEYS,
		.default_args.compact = {
		},
	},
	[FYAIVID_CONTEXT] = {
		.id	   = FYAIVID_CONTEXT,
		.name	   = "context",
		.configure = configure_context,
		.execute   = execute_context,
		.synopsis  = "context",
		.help	   = "Report context fill for the stored conversation: the model's\n"
			     "context window (catalogue), the last recorded call's token total,\n"
			     "and a tokenizer-free estimate (bytes/4) of the next request.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS |
			     FYAIVF_NEEDS_TRANSIENT_BUILDER,
		.default_args.context = {
		},
	},
	[FYAIVID_API] = {
		.id	   = FYAIVID_API,
		.name	   = "api",
		.configure = configure_api,
		.execute   = execute_api,
		.synopsis  = "api [responses|chat-completions|messages]",
		.help	   = "Show the resolved API grammar (endpoint, provider, model), or\n"
			     "switch to another one the provider offers and persist it as the\n"
			     "arena config `api` key (the project default). The interactive\n"
			     "/api switches the live session and persists the same way, so a\n"
			     "continuation resumes on the switched grammar.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.api = {
		},
	},
	[FYAIVID_LOG] = {
		.id	   = FYAIVID_LOG,
		.name	   = "log",
		.configure = configure_log,
		.execute   = execute_log,
		.synopsis  = "log [wire|stream|conversation|all] [start|stop|clear|view]",
		.help	   = "Control trace logging under .fyai/logs. With no arguments,\n"
			     "prints the current logging state. A single\n"
			     "action applies to all logs. Same backend as /log.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.log = {
			.arg = NULL,
		},
	},
	[FYAIVID_SANDBOX] = {
		.id	   = FYAIVID_SANDBOX,
		.name	   = "sandbox",
		.configure = configure_sandbox,
		.execute   = fyai_execute_config,
		.synopsis  = "sandbox [show|on|off|edit]",
		.help	   = "Show or configure the stored arena `sandbox` key.\n"
			     "  show      print the stored sandbox value\n"
			     "  on        store the canned sandbox policy\n"
			     "            { enabled: true,\n"
			     "              deny: [secrets, ~/.ssh],\n"
			     "              network: { ports: [443] } }\n"
			     "  off       store sandbox: false\n"
			     "  edit      edit the full stored config YAML",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.config = {
		},
	},
	[FYAIVID_AUTH] = {
		.id	   = FYAIVID_AUTH,
		.name	   = "auth",
		.configure = configure_auth,
		.execute   = fyai_auth_execute,
		.synopsis  = "auth [provider] [status|info|usage|login|logout] [options]",
		.help	   = "Manage machine-local ChatGPT subscription credentials.\n"
			     "  provider     subscription provider (currently openai)\n"
			     "  status       show login and credential health\n"
			     "  info         show subscription and account details\n"
			     "  usage        show live subscription limits and credits\n"
			     "  --json       emit status, info, or usage as JSON\n"
			     "  login        sign in with browser OAuth and a loopback callback\n"
			     "  --device-code use the headless device-code flow\n"
			     "  --no-browser print the URL without launching a browser\n"
			     "  --manual     paste the redirect URL back (for SSH/remote browsers)\n"
			     "  logout       revoke when possible and remove local credentials",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_STORAGE | FYAIVF_NO_REQUESTS |
			     FYAIVF_NEEDS_TRANSIENT_BUILDER,
		.default_args.auth = {
			.command = FYAI_AUTH_STATUS,
		},
	},
	[FYAIVID_SECRET] = {
		.id	   = FYAIVID_SECRET,
		.name	   = "secret",
		.configure = configure_secret,
		.execute   = fyai_secret_execute,
		.synopsis  = "secret [status [name]|set <name> [--stdin]|delete <name>]",
		.help	   = "Manage logical secrets without exposing their values.\n"
			     "Provider API keys conventionally use api-key/<provider>;\n"
			     "api_key: { type: auto } discovers that name automatically.\n"
			     "  status [name] show backend availability or entry presence\n"
			     "  set <name>    read from /dev/tty with echo disabled\n"
			     "  --stdin       read the value from stdin for scripts\n"
			     "  delete <name> remove an entry",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_STORAGE | FYAIVF_NO_REQUESTS,
		.default_args.secret = { .command = FYAI_SECRET_STATUS },
	},
	[FYAIVID_GC] = {
		.id	   = FYAIVID_GC,
		.name	   = "gc",
		.configure = configure_gc,
		.execute   = fyai_gc_storage,
		.synopsis  = "gc",
		.help	   = "Garbage-collect (compact) the arena. Requires arena quiescence.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_STORAGE | FYAIVF_NO_REQUESTS,
		.default_args.gc = {
		},
	},
	[FYAIVID_TOOL] = {
		.id	   = FYAIVID_TOOL,
		.name	   = "tool",
		.configure = configure_tool,
		.execute   = fyai_run_tool_verb,
		.synopsis  = "tool <name> [json-args]",
		.help	   = "Run one built-in tool as a sandboxed sub-execution of self\n"
			     "and print its result. Arguments are a JSON object, taken\n"
			     "from the command line or, if omitted, from stdin. The arena\n"
			     "sandbox policy (config `sandbox`) and environment\n"
			     "sanitization are applied to this process before the tool\n"
			     "runs. Tools: read_file, write_file, apply_patch, shell,\n"
			     "ask_user. Examples:\n"
			     "  fyai tool read_file '{\"path\": \"foo.c\"}'\n"
			     "  fyai tool shell '{\"command\": \"ls -l\"}'",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
		.default_args.tool = {
			.name = NULL,
			.args_json = NULL,
		},
	},
	[FYAIVID_HELP] = {
		.id	   = FYAIVID_HELP,
		.name	   = "help",
		.configure = configure_help,
		.execute   = execute_help,
		.synopsis  = "help [verb]",
		.help	   = "Without a verb, list all verbs and global options.\n"
			     "With a verb, describe that verb in detail.",
		.flags	   = FYAIVF_BATCH | FYAIVF_NO_STORAGE | FYAIVF_NO_REQUESTS,
		.default_args.help = {
			.verb = "",
		},
	},
};

const struct fyai_verb *fyai_find_verb(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fyai_verbs); i++)
		if (!strcmp(fyai_verbs[i].name, name))
			return &fyai_verbs[i];
	return NULL;
}

const struct fyai_verb *fyai_id_to_verb(enum fyai_verb_id id)
{
	const struct fyai_verb *v;

	if ((unsigned int)id >= ARRAY_SIZE(fyai_verbs))
		return NULL;

	v = &fyai_verbs[(unsigned int)id];
	return v->name && v->id == id ? v : NULL;
}

static int configure_help(int argc, char **argv, struct fyai_cfg *cfg)
{
	struct fyai_help_args *args = &cfg->cmd.args.help;
	const struct fyai_verb *v;

	if (argc < 2) {
		args->verb = "";
	} else {
		v = fyai_find_verb(argv[1]);
		if (!v) {
			fprintf(stderr, "help: unknown verb '%s'\n", argv[1]);
			return -1;
		}
		args->verb = v->name;
	}
	return 0;
}

static int execute_help(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_help_args *args = &cfg->cmd.args.help;
	const struct fyai_verb *v;

	v = fyai_find_verb(args->verb);
	if (!v) {
		fyai_usage(stdout, "fyai", cfg->color);
	} else {
		printf("fyai %s\n\n%s\n", v->synopsis, v->help);
	}
	return 0;
}

enum fyai_verb_id fyai_get_verb_id(const char *name)
{
	const struct fyai_verb *v;

	v = fyai_find_verb(name);
	return v ? v->id : FYAIVID_INVALID;
}

bool fyai_is_verb(const char *name)
{
	return fyai_find_verb(name) != NULL;
}

int fyai_configure(struct fyai_cfg *cfg, int argc, char *argv[])
{
	const struct fyai_verb *v;
	int rc;

	if (argc < 1 || !argv)
		return -1;

	v = fyai_find_verb(argv[0]);
	if (!v) {
		fprintf(stderr, "unknown verb '%s'\n", argv[0]);
		goto usage;
	}

	cfg->cmd.id = v->id;

	rc = v->configure(argc, argv, cfg);
	if (rc)
		goto usage;

	return 0;
usage:
	fyai_usage(stderr, "fyai", cfg->color);
	return -1;
}
