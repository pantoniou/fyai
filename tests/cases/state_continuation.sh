#!/bin/bash
# SPDX-License-Identifier: MIT
# State: a second invocation replays the first turn from the arena; --new
# starts a fresh conversation.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start state_continuation.json

run_fyai --set api=chat-completions --no-stream -u "$MOCK_URL/v1/chat/completions" \
	 -m mock-model "first question"
assert_status 0
assert_stdout_contains "First mock reply."

run_fyai --set api=chat-completions --no-stream -u "$MOCK_URL/v1/chat/completions" \
	 -m mock-model "second question"
assert_status 0
assert_stdout_contains "Second mock reply."

# the second request replays the whole first exchange
assert_request 1 'any(m.get("role") == "user" and m.get("content") == "first question" for m in r["body"]["messages"])'
assert_request 1 'any(m.get("role") == "assistant" and m.get("content") == "First mock reply." for m in r["body"]["messages"])'
assert_request 1 'any(m.get("role") == "user" and m.get("content") == "second question" for m in r["body"]["messages"])'

# both turns are in the canonical state
assert_state_contains "first question" dump state
assert_state_contains "Second mock reply." dump state
mock_stop 2

# --new drops the history
mock_start state_continuation.json
run_fyai --set api=chat-completions --no-stream -u "$MOCK_URL/v1/chat/completions" \
	 -m mock-model --new "fresh start"
assert_status 0
assert_request 0 'all(m.get("content") != "first question" for m in r["body"]["messages"])'
assert_request 0 'any(m.get("role") == "user" and m.get("content") == "fresh start" for m in r["body"]["messages"])'
mock_stop 1

pass
