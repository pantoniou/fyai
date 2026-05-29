/* SPDX-License-Identifier: MIT */
#ifndef FYAI_STREAM_H
#define FYAI_STREAM_H

#include "fyai.h"

fy_generic fyai_perform_streaming_request(struct fyai_ctx *ctx);
fy_generic fyai_perform_buffered_request(struct fyai_ctx *ctx);

#endif
