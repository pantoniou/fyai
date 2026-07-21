#!/bin/bash
# SPDX-License-Identifier: MIT
# token_extents on Anthropic Messages: no logprobs exist, so no logprobs
# params go on the wire and the fallback {text, pos} chunk extents (one per
# text_delta, no lp) land in turn metadata; usage stays exact.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start messages_stream.json

run_fyai --set api=messages --set api_url="$MOCK_URL/v1/messages" -m mock-model \
	 --set token_extents=true --set display/stats=true "stream please"
assert_status 0
assert_stdout_contains "Streamed messages answer."

assert_request 0 '"logprobs" not in r["body"]'
assert_request 0 '"top_logprobs" not in r["body"]'

# chunk extents: "Streamed "@0, "messages "@9, "answer."@18 - and no lp
assert_state_contains "tokens:" dump anchors
assert_state_contains "text: 'Streamed '" dump anchors
assert_state_contains "pos: 9" dump anchors
assert_state_contains "pos: 18" dump anchors
assert_state_absent "lp:" dump anchors

# exact output count still comes from usage
assert_stderr_contains "output=6"

mock_stop 1
pass
