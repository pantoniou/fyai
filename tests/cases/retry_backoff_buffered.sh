#!/bin/bash
# SPDX-License-Identifier: MIT
# The same retry, on the buffered transport. A buffered request presents
# nothing while it runs, so it takes the second retry path in fyai_stream.c.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start retry_backoff_buffered.json

run_fyai --set api=chat-completions --set tools=false --set display/stream=false \
	 --set retry/max_attempts=4 \
	 --set retry/initial_delay_ms=1 --set retry/max_delay_ms=10 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status 0
assert_stdout_contains "survived the storm"

# One attempt for each refusal, then the attempt that was answered.
mock_stop 3
pass
