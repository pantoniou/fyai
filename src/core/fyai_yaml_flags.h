/*
 * fyai_yaml_flags.h - parse flags of the YAML that fyai ingests
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_YAML_FLAGS_H
#define FYAI_YAML_FLAGS_H

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

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

#endif
