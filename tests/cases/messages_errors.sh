#!/bin/bash
# SPDX-License-Identifier: MIT
# Messages error paths: a non-2xx reply (529 overloaded) and a mid-stream
# error event must both fail cleanly, not crash.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

mock_start messages_errors.json
run_fyai --messages --no-stream -u "$MOCK_URL/v1/messages" \
	 -m mock-model "boom please"
assert_status_nonzero
[ "$FYAI_STATUS" -lt 128 ] || fail "crashed (status $FYAI_STATUS), not a clean error"
assert_stderr_contains "HTTP 529"
assert_stderr_contains "mock overloaded"
mock_stop 1

mock_start messages_error_event.json
run_fyai --messages -u "$MOCK_URL/v1/messages" -m mock-model "fail please"
assert_status_nonzero
[ "$FYAI_STATUS" -lt 128 ] || fail "crashed (status $FYAI_STATUS), not a clean error"
mock_stop 1

pass
