/*
 * fyai_transport_state.h - the credential transport component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_TRANSPORT_STATE_H
#define FYAI_TRANSPORT_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct fyai_cred;
struct fyai_event_source;
struct fyai_loop;
struct fyai_tclient;

/* The part this process has in the credential transport. */
enum fyai_transport_role {
	FYAI_TRANSPORT_NONE,		/* the process talks to the provider itself */
	FYAI_TRANSPORT_OWNER,		/* started the transport; holds the primary connection */
	FYAI_TRANSPORT_SUPERVISOR,	/* the supervisor outside a view; holds the primary connection */
	FYAI_TRANSPORT_SECONDARY,	/* an image of the run without the primary connection */
	FYAI_TRANSPORT_CHILD,		/* a sub-agent with the grant of its parent */
};

/*
 * The client of the credential transport and its control channel (see
 * fyai_xfer.h). role is NONE while client is NULL. A role of OWNER or
 * SUPERVISOR states the profiles; only the OWNER ends the transport. The
 * component stands on the loop for the source of its control channel and on
 * the credential component for the grants. The owner is struct fyai_app.
 */
struct fyai_transport_state {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_cred *cred;

	struct fyai_tclient *client;
	enum fyai_transport_role role;
	int ctl;			/* control channel; -1 if none */
	pid_t pid;
	struct fyai_event_source *src;	/* drains the control channel */
	uint64_t exec;			/* the execution the transport knows us as */
	long long seq;
	char why[160];			/* the cause of a failed control request */
	/* Profile names that this execution has stated and been granted. */
	char names[64][64];
	unsigned int nnames;
	/*
	 * The configuration generation the supervisor last stated. stated is
	 * a latch that tells whether gen is meaningful; it is not a phase.
	 */
	unsigned int gen;
	bool stated;
	int envfd;			/* read end of a credential grant; <= 2: none */
};

void fyai_transport_state_init(struct fyai_transport_state *state,
			       struct fyai_loop *loop, struct fyai_cred *cred);
/* The source that drains the control channel is the parent's. */
void fyai_transport_state_abandon(struct fyai_transport_state *state);

#endif
