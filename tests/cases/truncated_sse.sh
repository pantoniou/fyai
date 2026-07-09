#!/bin/bash
# SPDX-License-Identifier: MIT
# An SSE stream cut off before completion ([DONE] for chat,
# response.completed for responses) must fail cleanly, not crash.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

mock_start truncated_sse_chat.json
run_fyai --set api=chat-completions -u "$MOCK_URL/v1/chat/completions" \
	 -m mock-model "cut me off"
assert_status_nonzero
[ "$FYAI_STATUS" -lt 128 ] || fail "crashed (status $FYAI_STATUS), not a clean error"
mock_stop 1

mock_start truncated_sse_responses.json
run_fyai --set api=responses -u "$MOCK_URL/v1/responses" -m mock-model "cut me off"
assert_status_nonzero
[ "$FYAI_STATUS" -lt 128 ] || fail "crashed (status $FYAI_STATUS), not a clean error"
mock_stop 1

pass
