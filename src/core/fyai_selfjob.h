/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SELFJOB_H
#define FYAI_SELFJOB_H

#include <stdbool.h>

struct fyai_ctx;
struct fyai_selfjob;

/*
 * A run of fyai itself as a child on the event loop, such as `fyai decide`.
 * The child has /dev/null as standard input and error, and its standard
 * output is collected. Under credential isolation it gets channels of its
 * own and the transport admits it, as the title child does, so it makes
 * model requests without seeing a key.
 *
 * @done is called once, from the loop: with the exit status (-1 when the
 * child was killed at its time limit, or could not be watched) and the
 * output, NUL-terminated and valid during the call. It is not called after
 * fyai_selfjob_cancel(). The job frees itself after @done.
 */
typedef void (*fyai_selfjob_done_fn)(void *data, int status, const char *out);

struct fyai_selfjob *fyai_selfjob_start(struct fyai_ctx *ctx,
					const char *const *argv,
					long long timeout_ms,
					fyai_selfjob_done_fn done, void *data);

/*
 * Run `fyai decide --spec @spec --input @input` as a job: @spec is a decider
 * that this process resolved, as JSON, and @input one JSON value. The child
 * is given the branch, the model, the grammar and the endpoint of this
 * process, since the settings of an invocation are not stored. Its output
 * is one JSON line.
 */
struct fyai_selfjob *fyai_selfjob_decide_start(struct fyai_ctx *ctx,
					       const char *spec,
					       const char *input,
					       long long timeout_ms,
					       fyai_selfjob_done_fn done,
					       void *data);

/* Stop the job: kill the child, and do not call its @done. */
void fyai_selfjob_cancel(struct fyai_selfjob *job);

#endif
