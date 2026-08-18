#!/bin/bash
# SPDX-License-Identifier: MIT
# A spent quota reads like a rate limit but does not refill on the timescale of
# a backoff. It must be reported at once, and its message must survive: an
# in-stream error is the only place the reason is written.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start retry_stream_error_fatal.json

run_fyai --set api=chat-completions --set tools=false \
	 --set retry/max_attempts=4 \
	 --set retry/initial_delay_ms=1 --set retry/max_delay_ms=10 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status_nonzero
assert_stderr_contains "exceeded your current quota"

mock_stop 1
pass
