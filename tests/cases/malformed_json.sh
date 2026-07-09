#!/bin/bash
# SPDX-License-Identifier: MIT
# A 200 reply with a garbage body must fail cleanly, not crash.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

mock_start malformed.json
run_fyai --set api=chat-completions --set display/stream=false -u "$MOCK_URL/v1/chat/completions" \
	 -m mock-model "garbage please"
assert_status_nonzero
[ "$FYAI_STATUS" -lt 128 ] || fail "crashed (status $FYAI_STATUS), not a clean error"
mock_stop 1

pass
