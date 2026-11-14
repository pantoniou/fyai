/*
 * fyai_agent_state.h - the sub-agent identity component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_AGENT_STATE_H
#define FYAI_AGENT_STATE_H

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_agents;
struct fyai_branch_state;
struct fyai_diag;

/*
 * The registry of the sub-agents that this run started, and the identity of
 * the run when it is a sub-agent: its execution and parent numbers and the
 * spawn state it received. The component reads the branch cursor and reports
 * into the diagnostics. The owner is struct fyai_app.
 */
struct fyai_agent_state {
	/* Dependencies. */
	struct fyai_branch_state *bstate;
	struct fyai_diag *diag;

	struct fyai_agents *registry;
	long long execution, parent;
	/*
	 * The spawn state an executed sub-agent child received, held in the
	 * configuration builder: the child serves one call and exits.
	 */
	fy_generic spawn;
};

void fyai_agent_state_init(struct fyai_agent_state *as,
			   struct fyai_branch_state *bstate,
			   struct fyai_diag *diag);

#endif
