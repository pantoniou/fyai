#!/bin/bash
# SPDX-License-Identifier: MIT
# A provider caches a prefix of the request. A tool turn of responses goes back
# as the provider sent it, and each request starts with the one before; the
# conversation then continues on chat-completions with the same property.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start grammar_cache_prefix_responses.json

FROM="--set api=responses --set api_url=$MOCK_URL/v1/responses"
TO="--set api=chat-completions --set api_url=$MOCK_URL/v1/chat/completions"

# shellcheck disable=SC2086
run_fyai $FROM --set display/stream=false --set tools=true \
	 -m mock-model "read both files"
assert_status 0
assert_stdout_contains "Both files are missing."

# shellcheck disable=SC2086
run_fyai $TO --set display/stream=false --set tools=true \
	 -m mock-model "and then?"
assert_status 0
assert_stdout_contains "Second answer."

# shellcheck disable=SC2086
run_fyai $TO --set display/stream=false --set tools=true \
	 -m mock-model "and after that?"
assert_status 0
assert_stdout_contains "Third answer."

mock_stop 4

"$PYTHON" "$TESTS_DIR/grammar_prefix.py" "$TEST_DIR/requests.jsonl" \
	"$TESTS_DIR/scenarios/grammar_cache_prefix_responses.json" responses chat-completions ||
	fail "the requests do not keep the cached prefix"
pass
