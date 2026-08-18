#!/bin/bash
# SPDX-License-Identifier: MIT
# A non-2xx reply must fail cleanly (no crash) with the error surfaced, and
# must not commit a bogus assistant turn - in both buffered and streamed mode.
# Retry is off: what is under test is how a failure is reported, not how often
# a transient status is tried again. See the retry_* cases for that.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

mock_start errors.json
run_fyai --set api=chat-completions --set display/stream=false \
	 --set retry/max_attempts=1 --set api_url="$MOCK_URL/v1/chat/completions" \
	 -m mock-model "boom please"
assert_status_nonzero
[ "$FYAI_STATUS" -lt 128 ] || fail "crashed (status $FYAI_STATUS), not a clean error"
assert_stderr_contains "HTTP 500"
assert_stderr_contains "mock boom"
mock_stop 1

mock_start errors.json
run_fyai --set api=chat-completions --set retry/max_attempts=1 \
	 --set api_url="$MOCK_URL/v1/chat/completions" \
	 -m mock-model "boom please"
assert_status_nonzero
[ "$FYAI_STATUS" -lt 128 ] || fail "crashed (status $FYAI_STATUS), not a clean error"
assert_stderr_contains "HTTP 500"
mock_stop 1

pass
