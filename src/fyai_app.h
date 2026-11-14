/*
 * fyai_app.h - the components that the whole program shares
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_APP_H
#define FYAI_APP_H

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_loop.h"

/*
 * The root of the program: it owns each component that more than one part of
 * the program uses, and it outlives the context. main() declares one and
 * every later object borrows from it (doc/context-components.md).
 *
 * A component is a struct that owns its state and names its dependencies as
 * pointers to other components, set before it is used. fyai_app_init() makes
 * the components in dependency order and wires the pointers;
 * fyai_app_cleanup() destroys them in the reverse order.
 */
struct fyai_app {
	struct fyai_diag diag;		/* depends on nothing */
	struct fyai_loop loop;		/* depends on diag */
	struct fyai_cfg cfg;		/* depends on diag */
};

/*
 * Make the components of @app and wire their dependencies. @app is zeroed
 * first. Returns 0, or -1 when a component cannot be made; nothing is left
 * to release then.
 */
int fyai_app_init(struct fyai_app *app);

/* Release what the components hold. The diagnostics are drained first. */
void fyai_app_cleanup(struct fyai_app *app);

#endif
