/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SYSTEMONE_H
#define FYAI_SYSTEMONE_H

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/*
 * The System One API of decision models, such as TypeSafe's Jev: a POST of
 * {model, state, questions} whose answer is {answers: {ID: answer}}, each a
 * choice, a score or a noul with its probabilities. It generates no text.
 */

/*
 * The key of @provider ({name, key_env, ...}, from fyai_decider_systemone):
 * its variable, else the secret fyai:api-key/NAME. NULL when there is none.
 */
const char *fyai_systemone_key(struct fyai_ctx *ctx,
			       struct fy_generic_builder *gb,
			       fy_generic provider);

/*
 * Send @request to @url, or to the URL of @provider, with @key, and wait on
 * the event loop of @ctx; a transient failure is tried again as a model
 * request is. The answers mapping, in @gb, or fy_invalid with the reason
 * in *@whyp.
 */
fy_generic fyai_systemone_ask(struct fyai_ctx *ctx,
			      struct fy_generic_builder *gb,
			      fy_generic provider, const char *url,
			      const char *key, fy_generic request,
			      const char **whyp);

#endif
