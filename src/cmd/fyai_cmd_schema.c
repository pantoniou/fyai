/*
 * fyai_cmd_schema.c - handler of the schema command
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The command definitions are the one source of the commands. The export
 * gives a front end what it needs to offer them with no list of its own: the
 * names, the descriptions and the JSON Schema of the arguments, for one
 * surface, or for all of them, and without the hidden commands and options.
 */

#define SCHEMA_ALL ((enum fyai_cmd_surface)-1)

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_catalog.h"
#include "fyai_config.h"

#define FYAI_MODULE FYAIEM_CONFIG

/* The members of a definition that a front end reads. */
static const char *const export_keys[] = {
	"command", "aliases", "title", "description", "default", "fallback",
	"examples", "see_also", "setting", "flags", NULL
};

/* The argument schema without the options another surface or nobody uses. */
static fy_generic schema_arguments(struct fy_generic_builder *gb,
				   fy_generic args,
				   enum fyai_cmd_surface surface)
{
	fy_generic props, kept, p, v, out;
	const char *name;

	props = fy_get(args, "properties", fy_invalid);
	if (!fy_is_mapping(props))
		return args;
	kept = fy_map_empty;
	fy_foreach_key_value(name, p, props) {
		if (fy_get(p, "x-fyai-hidden", false))
			continue;
		v = fy_get(p, "x-fyai-surfaces", fy_invalid);
		if (surface != SCHEMA_ALL && fy_is_valid(v) &&
		    !fyai_cmd_seq_has(v, fyai_cmd_surface_name(surface)))
			continue;
		kept = fy_assoc(gb, kept, name, p);
	}
	out = fy_assoc(gb, args, "properties", kept);
	return out;
}

static fy_generic schema_command(struct fy_generic_builder *gb, fy_generic def,
				 fy_generic inherited,
				 enum fyai_cmd_surface surface)
{
	fy_generic out, v, sub, subs, surfaces;
	size_t i;

	if (fy_get(def, "hidden", false) ||
	    (surface != SCHEMA_ALL && !fyai_cmd_def_on(def, inherited, surface)))
		return fy_invalid;
	surfaces = fyai_cmd_def_surfaces(def, inherited);
	out = fy_map_empty;
	for (i = 0; export_keys[i]; i++) {
		v = fy_get(def, export_keys[i], fy_invalid);
		if (fy_is_valid(v))
			out = fy_assoc(gb, out, export_keys[i], v);
	}
	if (fy_is_valid(surfaces))
		out = fy_assoc(gb, out, "surfaces", surfaces);
	else
		out = fy_assoc(gb, out, "surfaces",
			       fy_sequence(gb, "cli", "session"));
	v = fy_get(def, "arguments", fy_invalid);
	if (fy_is_valid(v))
		out = fy_assoc(gb, out, "arguments",
			       schema_arguments(gb, v, surface));
	subs = fy_seq_empty;
	fy_foreach(sub, fy_get(def, "commands", fy_invalid)) {
		v = schema_command(gb, sub, surfaces, surface);
		if (fy_is_valid(v))
			subs = fy_append(gb, subs, v);
	}
	if (!fy_empty(subs))
		out = fy_assoc(gb, out, "commands", subs);
	return out;
}

static fy_generic schema_commands(struct fyai_cmd_call *call,
				  enum fyai_cmd_surface surface)
{
	struct fy_generic_builder *gb = call->gb;
	fy_generic doc, def, v, cmds;

	doc = fyai_cmd_registry();
	fyai_error_check(fyai_cmd_call_ctx(call), fy_is_valid(doc), err,
			 "the command definitions do not load: %s",
			 fyai_cmd_registry_why());
	cmds = fy_seq_empty;
	fy_foreach(def, fy_get(doc, "commands", fy_invalid)) {
		v = schema_command(gb, def, fy_invalid, surface);
		if (fy_is_valid(v))
			cmds = fy_append(gb, cmds, v);
	}
	return fy_mapping(gb, "surface", surface == SCHEMA_ALL ? "all" :
			  fyai_cmd_surface_name(surface),
			  "commands", cmds);
err:
	return fy_invalid;
}

int fyai_cmd_schema(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *what = fyai_cmd_arg_str(call, "what");
	const char *surface = fyai_cmd_arg_str(call, "surface");

	if (!strcmp(what, "config"))
		return fyai_cmd_config_schema(call, result);
	if (!strcmp(what, "catalog"))
		return fyai_cmd_catalog_schema(call, result);
	*result = schema_commands(call, !strcmp(surface, "cli") ?
				  FYAI_CMD_CLI : !strcmp(surface, "session") ?
				  FYAI_CMD_SESSION : SCHEMA_ALL);
	return fy_is_valid(*result) ? 0 : -1;
}
