/*
 * fyai_toolspec.h - the tool specification component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_TOOLSPEC_H
#define FYAI_TOOLSPEC_H

#include <stdbool.h>
#include <stdint.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_skills;

/*
 * The built-in tool specification in the configuration builder, its
 * generation, and the skills and the allow list that it describes. The
 * component reads the skills component. The owner is struct fyai_app.
 */
struct fyai_toolspec {
	/* Dependency. */
	struct fyai_skills *skills;

	fy_generic spec;
	unsigned int generation;
	bool agent_child;
	bool agent_restricted;
	/* The skills and the tool allow list that spec describes. */
	uint64_t spec_skills;
	uint64_t spec_allow;
	/*
	 * The tools that a sub-agent run for a skill may call, a sequence of
	 * canonical names, or fy_invalid for every tool.
	 */
	fy_generic allow;
};

void fyai_toolspec_init(struct fyai_toolspec *ts, struct fyai_skills *skills);

#endif
