#!/bin/bash
# SPDX-License-Identifier: MIT
# A truncated provider payload must be reported by fyai, in one diagnostic that
# says what failed and why. The parser must not write its own report: it names
# the anonymous input buffer by address, which is outside the sink and tells
# the user nothing about which request produced it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start malformed_json.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=false --set retry/max_attempts=1 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "hello"
assert_status_nonzero
# fyai's own diagnostic, naming what was being read.
assert_stderr_contains "could not read the provider response"
# The parser's reason is folded into it, with the place.
assert_stderr_contains "line"
# The raw parser report must be gone: it named the buffer by address.
grep -q "<memory-@" "$TEST_DIR/stderr" &&
	fail "the parser wrote its own report outside the sink"

mock_stop 1
pass
