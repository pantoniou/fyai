#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent that waits out a rate limit must report the wait through its
# progress channel, so it reaches the parent's band for this delegation. A
# status line from the child would land on the parent's terminal raw, beside
# the display instead of inside it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_retry_progress.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=true --set retry/max_attempts=4 \
	 --set retry/initial_delay_ms=1 --set retry/max_delay_ms=10 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate the job"
assert_status 0
assert_stdout_contains "The sub-agent reported."

# The child's wait must not reach the terminal as a loose line.
grep -q "retrying in" "$TEST_DIR/stderr" &&
	fail "the sub-agent wait leaked to the parent terminal"

mock_stop 4
pass
