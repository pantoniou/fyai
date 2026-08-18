#!/bin/bash
# SPDX-License-Identifier: MIT
# A provider that answers 429 or 503 is busy, not broken. The request waits out
# a backoff and is made again, and the turn completes without the user seeing a
# failure. Delays are set small: the schedule is under test, not the clock.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start retry_backoff.json

run_fyai --set api=chat-completions --set tools=false \
	 --set retry/max_attempts=4 \
	 --set retry/initial_delay_ms=1 --set retry/max_delay_ms=10 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status 0
assert_stdout_contains "survived the storm"

# One attempt for each refusal, then the attempt that was answered.
mock_stop 3
pass
