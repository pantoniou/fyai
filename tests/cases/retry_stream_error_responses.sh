#!/bin/bash
# SPDX-License-Identifier: MIT
# The same in-stream rate limit in the Responses grammar, where it arrives as a
# typed error event rather than an error object on the chunk.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start retry_stream_error_responses.json

run_fyai --set api=responses --set tools=false \
	 --set retry/max_attempts=4 \
	 --set retry/initial_delay_ms=1 --set retry/max_delay_ms=10 \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model "hello"
assert_status 0
assert_stdout_contains "served at last"

mock_stop 2
pass
