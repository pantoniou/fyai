/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TEST_DIAG_H
#define FYAI_TEST_DIAG_H

#include <stdlib.h>

#include "fyai.h"
#include "fyai_diag.h"

/*
 * A configuration that stands alone owns its diagnostics in a test: the sink
 * is made on the heap and the configuration points at it, as the program
 * points the configuration at the sink of struct fyai_app.
 */
static inline int fyai_test_diag_setup(struct fyai_cfg *cfg)
{
	cfg->diag = calloc(1, sizeof(*cfg->diag));
	if (!cfg->diag)
		return -1;
	return fyai_diag_setup(cfg->diag);
}

static inline void fyai_test_diag_cleanup(struct fyai_cfg *cfg)
{
	if (!cfg->diag)
		return;
	fyai_diag_cleanup(cfg->diag);
	free(cfg->diag);
	cfg->diag = NULL;
}

#endif
