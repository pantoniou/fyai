/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SYSTEMONE_H
#define FYAI_SYSTEMONE_H

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct fyai_cfg;

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
 * The credential source of @provider under credential isolation, which only
 * the transport resolves: "env:VAR|secret:api-key/NAME". In @gb.
 */
const char *fyai_systemone_credential(struct fy_generic_builder *gb,
				      fy_generic provider);

/* The fixed request headers of a transport profile; NULL-terminated or NULL. */
const char *const *fyai_systemone_headers(bool images);

/*
 * The endpoints of the decision models that the deciders of @cfg use, in
 * @gb: a sequence of {url, credential}, each credential a source of
 * fyai_systemone_credential(). Empty when no decider names a System One model.
 */
fy_generic fyai_systemone_endpoints(struct fyai_cfg *cfg,
				    struct fy_generic_builder *gb);

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
