#!/bin/bash
# SPDX-License-Identifier: MIT
# A 400 is the provider's answer, not a passing condition. Repeating it would
# only ask the same rejected question again, so it must fail on the first try.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start retry_not_transient.json

run_fyai --set api=chat-completions --set tools=false \
	 --set retry/max_attempts=4 \
	 --set retry/initial_delay_ms=1 --set retry/max_delay_ms=10 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status_nonzero
assert_stderr_contains "400"

mock_stop 1
pass
