/* SPDX-License-Identifier: MIT */
#ifndef FYAI_STREAM_H
#define FYAI_STREAM_H

#include "fyai.h"

struct stream_response;
typedef struct stream_response fyai_stream_request;
struct fyai_buffered_request;

enum fyai_stream_state {
	FYAISS_NEW,
	FYAISS_SUBMITTED,
	FYAISS_STREAMING,
	FYAISS_RETRYING,
	FYAISS_CANCELLING,
	FYAISS_COMPLETED,
	FYAISS_CANCELLED,
	FYAISS_FAILED,
};

typedef void (*fyai_stream_complete_fn)(fyai_stream_request *request,
					void *userdata);
typedef void (*fyai_stream_tool_fn)(fyai_stream_request *request,
				    fy_generic tool_call, size_t index,
				    void *userdata);
typedef void (*fyai_buffered_complete_fn)(
		struct fyai_buffered_request *request, void *userdata);

struct fyai_stream_callbacks {
	fyai_stream_complete_fn complete;
	fyai_stream_tool_fn tool;
	void *userdata;
};

fyai_stream_request *
fyai_stream_request_submit(struct fyai_ctx *ctx,
			   const struct fyai_stream_callbacks *callbacks);
void fyai_stream_request_cancel(fyai_stream_request *request);
enum fyai_stream_state
fyai_stream_request_state(const fyai_stream_request *request);
bool fyai_stream_request_done(const fyai_stream_request *request);
fy_generic fyai_stream_request_collect(const fyai_stream_request *request);
void fyai_stream_request_destroy(fyai_stream_request *request);

struct fyai_buffered_request *
fyai_buffered_request_submit(struct fyai_ctx *ctx,
			     fyai_buffered_complete_fn complete,
			     void *userdata);
void fyai_buffered_request_cancel(struct fyai_buffered_request *request);
bool fyai_buffered_request_done(
		const struct fyai_buffered_request *request);
fy_generic fyai_buffered_request_collect(
		const struct fyai_buffered_request *request);
void fyai_buffered_request_destroy(struct fyai_buffered_request *request);

fy_generic fyai_perform_streaming_request(struct fyai_ctx *ctx);
fy_generic fyai_perform_streaming_request_tools(
		struct fyai_ctx *ctx, fyai_stream_tool_fn tool,
		void *userdata);
fy_generic fyai_perform_buffered_request(struct fyai_ctx *ctx);

#endif
