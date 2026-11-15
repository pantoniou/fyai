/* SPDX-License-Identifier: MIT */
#ifndef FYAI_MODEL_H
#define FYAI_MODEL_H

#include <stdbool.h>

#include "fyai.h"

struct fyai_model_step;
struct fyai_tool_job_group;

enum fyai_model_step_state {
	FYAIMSS_NEW,
	FYAIMSS_BUILDING,
	FYAIMSS_REQUEST_PENDING,
	FYAIMSS_RETRYING,
	FYAIMSS_COMPLETED,
	FYAIMSS_CANCELLED,
	FYAIMSS_FAILED,
};

typedef void (*fyai_model_step_complete_fn)(
		struct fyai_model_step *step, void *userdata);

struct fyai_model_step *
fyai_model_step_submit(struct fyai_ctx *ctx, fy_generic turn,
		       fy_generic previous,
		       struct fyai_tool_job_group *tool_group,
		       fyai_model_step_complete_fn complete,
		       void *userdata);
void fyai_model_step_cancel(struct fyai_model_step *step);
enum fyai_model_step_state
fyai_model_step_state(const struct fyai_model_step *step);
bool fyai_model_step_done(const struct fyai_model_step *step);
fy_generic fyai_model_step_collect(const struct fyai_model_step *step);
void fyai_model_step_destroy(struct fyai_model_step *step);

#endif
