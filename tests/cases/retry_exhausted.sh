#!/bin/bash
# SPDX-License-Identifier: MIT
# The attempt budget is a bound. When it runs out the last failure is reported
# as the error it is, and no further request is made.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start retry_exhausted.json

run_fyai --set api=chat-completions --set tools=false \
	 --set retry/max_attempts=2 \
	 --set retry/initial_delay_ms=1 --set retry/max_delay_ms=10 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status_nonzero
assert_stderr_contains "429"

# max_attempts=2 means the first attempt and one retry. No third request.
mock_stop 2
pass
