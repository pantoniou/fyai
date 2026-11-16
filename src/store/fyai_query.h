/* SPDX-License-Identifier: MIT */
#ifndef FYAI_QUERY_H
#define FYAI_QUERY_H

#include "fyai.h"

/*
 * A worker queries its arena for clients without one. Each page carries
 * the revision of its source data.
 *
 * A request is a mapping {what, params, after, limit, if_rev}. An answer is a
 * mapping, one of
 *
 *	{what, rows, next, total, rev}	a page; @next is a cursor or null
 *	{unchanged: true, rev}		the data still has the rev of if_rev
 *	{error: {code, message}}	code is one of the FYAI_QUERY_* below
 *
 * A revision encodes the source arena value as an opaque integer. Clients
 * compare revisions only for equality.
 */

#define FYAI_QUERY_LIMIT_DEFAULT	200
#define FYAI_QUERY_LIMIT_MAX		1000
/* Maximum estimated size of the rows in one reply. */
#define FYAI_QUERY_BYTES_MAX		(1024u * 1024u)

#define FYAI_QUERY_UNSUPPORTED	"unsupported"	/* no such `what` */
#define FYAI_QUERY_BAD_REQUEST	"bad_request"	/* an argument is malformed */
#define FYAI_QUERY_STALE_CURSOR	"stale_cursor"	/* restart pagination */
#define FYAI_QUERY_TOO_LARGE	"too_large"	/* row exceeds byte limit */

/* What the queries read. */
struct fyai_query_source {
	fy_generic branches;	/* the branch table of the root, or fy_invalid */
	const char *cwd;	/* the directory that a session belongs to, or NULL */
};

/* Return supported query names and set @n to their count. */
const char *const *fyai_query_names(size_t *n);

/* Answer @request from @src. The answer lives in @gb. */
fy_generic fyai_query_answer(struct fy_generic_builder *gb,
			     const struct fyai_query_source *src,
			     fy_generic request);

/* Answer @request from the arena of @ctx, in the directory of the process. */
fy_generic fyai_query(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		      fy_generic request);

#endif
