#!/bin/bash
# SPDX-License-Identifier: MIT
# token_extents on Chat streaming: logprob entries delimit tokens exactly;
# `bytes` length (not char count) advances the position; extents land in
# turn metadata under `tokens`.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_stream_logprobs.json

run_fyai --chat-completions -u "$MOCK_URL/v1/chat/completions" \
	 -m mock-model --token-extents "stream please"
assert_status 0
assert_stdout_contains "Hi é!"

assert_request 0 'r["body"]["logprobs"] is True'
assert_request 0 'r["body"]["stream"] is True'

# per-token extents: "Hi"@0, " "@2, "é"@3 (2 bytes), "!"@5, with lp values
assert_state_contains "tokens:" dump anchors
assert_state_contains "text: Hi" dump anchors
assert_state_contains "lp: -0.25" dump anchors
assert_state_contains "lp: -1.5" dump anchors
assert_state_contains "pos: 3" dump anchors
assert_state_contains "pos: 5" dump anchors

mock_stop 1
pass
