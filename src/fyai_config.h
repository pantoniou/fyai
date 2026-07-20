/*
 * fyai_config.h - layered configuration loading for fyai
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_CONFIG_H
#define FYAI_CONFIG_H

#include <libfyaml/libfyaml-generic.h>

#include "fyai.h"

/*
 * Parse flags for every human-authored YAML document fyai ingests (config
 * files, the config sample, an editor round-trip, a catalogue import).
 * FYOPPF_KEEP_COMMENTS captures comments and FYOPPF_KEEP_STYLE captures the
 * original scalar styles (quoted/literal/plain) into the generic, so they
 * survive the content-addressed arena and a config edit/export can reproduce
 * the comments and scalar spelling the user wrote.
 */
#define FYAI_YAML_PARSE_FLAGS \
	(FYOPPF_DISABLE_DIRECTORY | FYOPPF_MODE_YAML_1_2 | \
	 FYOPPF_KEEP_COMMENTS | FYOPPF_KEEP_STYLE)

/*
 * Emit flags for every human-editable YAML document fyai writes back out
 * (config export, the config edit tempfile). FYOPEF_OUTPUT_COMMENTS
 * reproduces the comments captured by FYAI_YAML_PARSE_FLAGS.
 */
#define FYAI_YAML_EMIT_FLAGS \
	(FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_YAML_1_2 | \
	 FYOPEF_STYLE_PRETTY | FYOPEF_WIDTH_INF | FYOPEF_OUTPUT_COMMENTS)

/*
 * Load layered configuration into @cfg.
 *
 * Up to three config files are consulted: the user config
 * ($XDG_CONFIG_HOME/fyai/config.yaml, else $HOME/.config/fyai/config.yaml),
 * the repository config (nearest .fyai/config.yaml walking up from cwd), and
 * an explicit @cli_config path given on the command line. The explicit file
 * is the most specific file layer (it overrides the user and repository
 * files) and, unlike the discovered files, must exist when named.
 *
 * Each layer sets top-level keys (`model`, `system_prompt`, `api_key` as a
 * `{ type: env, value: NAME }` mapping, ...) and the nested `display:` group.
 * The `model` may carry a `provider/` prefix; the catalogue resolves it to a
 * provider endpoint/grammar/wire-id later, in fyai_config_setup.
 *
 * Layers are applied lowest-to-highest precedence:
 *
 *   1. whatever @cfg already holds (built-in defaults / environment)
 *   2. top-level config keys (user file, then repository, then explicit)
 *
 * If @cli_env is non-NULL it names a .env file (KEY=VALUE / export KEY=VALUE,
 * optionally quoted, no variable substitution). Only variables fyai actually
 * uses are exported into the environment: the OPENAI_* names it reads
 * directly plus every variable referenced by a `{ type: env, value: NAME }`
 * mapping in the configs. It is applied before secrets are resolved.
 *
 * Command-line options are applied by the caller after this returns and
 * therefore take precedence over every file layer.
 *
 * Strings stored into @cfg point into @gb and remain valid for the
 * builder's lifetime; @gb must outlive any use of @cfg.
 *
 * Returns 0 on success (including when no config file exists), -1 on a
 * hard error such as a malformed file.
 */
int fyai_config_load(struct fyai_cfg *cfg,
		     const char *cli_config, const char *cli_env);

/* `fyai config` verb backends. */
int fyai_config_show(struct fyai_cfg *cfg);
int fyai_config_get(struct fyai_ctx *ctx, const char *key);
int fyai_config_set(struct fyai_ctx *ctx, const char *key, const char *value);
int fyai_config_delete(struct fyai_ctx *ctx, const char *key);
int fyai_apply_config_ops(struct fyai_ctx *ctx);
int fyai_config_import(struct fyai_ctx *ctx, const char *path);
int fyai_config_export(struct fyai_ctx *ctx, const char *path);
int fyai_config_edit(struct fyai_ctx *ctx);

/*
 * Rebuild the live derived config cache from the arena config after an
 * in-session mutation (/config set|delete|edit|import). Re-merges config_doc
 * from the freshly published arena_config, re-runs the apply_config pass,
 * reloads the markdown styling, and re-resolves the model, so a REPL config
 * change takes effect immediately instead of only on the next process.
 */
int fyai_config_rederive(struct fyai_ctx *ctx);

/*
 * Re-derive the read-only `catalog:` block on @config_doc: the full models[]
 * entry for the document's `model` (display_name, context_window,
 * capabilities, open_source, ...) plus `canonical_provider`. Removed
 * entirely when the model is unknown to @catalog. Used both on every config
 * commit path (via config_doc_sanitize) and by `catalog import`, which must
 * re-sync the existing arena config against the newly ingested catalogue.
 */
fy_generic fyai_config_sync_catalog(struct fy_generic_builder *gb,
				    fy_generic catalog, fy_generic config_doc);

fy_generic fyai_config_validate_report(struct fyai_cfg *cfg, fy_generic doc,
				       const char *origin);
int fyai_config_validate_document(struct fyai_cfg *cfg, fy_generic doc,
				       const char *origin);
/*
 * Report every problem in a validation report as a single diagnostic. An
 * invalid config is one failure however many things are wrong with it: raised
 * one by one, all but the first would be demoted behind it and lost.
 */
void fyai_config_report_problems(struct fyai_cfg *cfg, fy_generic report);

/*
 * The vendored config document JSON Schema (data/config.schema.yaml,
 * embedded at build time), parsed once into @gb. Structural/type/enum
 * validation only, over and above fyai_config_validate_report's semantic
 * checks - see fyai_schema.h.
 */
fy_generic fyai_config_schema(struct fy_generic_builder *gb);

/*
 * Validate @doc against fyai_config_schema(), printing problems to stderr
 * (fyai_schema_report_print convention). Returns 0 when it matches.
 */
int fyai_config_validate_schema(struct fyai_cfg *cfg, fy_generic doc,
				const char *origin);

void fyai_config_set_defaults(struct fyai_cfg *cfg);

/*
 * Discover repo-scoped instruction files (AGENTS.md/CLAUDE.md) from the
 * global config dir and the project root down to the cwd. Returns a malloc'd
 * combined document (caller frees) or NULL when none exist.
 */
char *fyai_project_instructions(void);
char *fyai_discover_project_root(void);

/*
 * Resolve `model` against the effective catalogue (provider pin, endpoint,
 * wire id, capability check, max_tokens default, provider env api-key
 * fallback). Re-runnable for a mid-session model switch once the caller
 * clears api_url/provider (and api_key unless explicit).
 */
int fyai_config_resolve_model(struct fyai_cfg *cfg);

/* Reject/downgrade options with no Messages-API mapping; no-op otherwise. */
int fyai_config_messages_gate(struct fyai_cfg *cfg);

int fyai_config_setup(struct fyai_cfg *cfg, int argc, char *argv[]);
void fyai_config_cleanup(struct fyai_cfg *cfg);

#endif
