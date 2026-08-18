#!/bin/bash
# SPDX-License-Identifier: MIT
# A rate limit does not always arrive as HTTP 429. On a streamed endpoint the
# status is 200 and the limit arrives as an error object inside the stream, so
# the status alone cannot decide. The provider's error code must, and the
# request must be made again the same way a 429 is.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start retry_stream_error.json

run_fyai --set api=chat-completions --set tools=false \
	 --set retry/max_attempts=4 \
	 --set retry/initial_delay_ms=1 --set retry/max_delay_ms=10 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status 0
assert_stdout_contains "served at last"
# The wait is reported, and it names what the provider said. A status 0 above
# is what proves the held cause was never raised as an error.
assert_stderr_contains "retrying in"
assert_stderr_contains "Rate limit reached"

mock_stop 2
pass
