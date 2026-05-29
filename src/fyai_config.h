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
