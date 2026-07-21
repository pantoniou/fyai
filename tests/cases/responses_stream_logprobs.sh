#!/bin/bash
# SPDX-License-Identifier: MIT
# token_extents on Responses streaming: the request carries top_logprobs and
# the logprobs include; per-token extents from the delta logprobs arrays land
# in turn metadata and stay consistent with the completed response text.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_stream_logprobs.json

run_fyai --set api=responses --set api_url="$MOCK_URL/v1/responses" \
	 -m mock-model --set token_extents=true "stream please"
assert_status 0
assert_stdout_contains "For sure."

assert_request 0 'r["body"]["top_logprobs"] == 0'
assert_request 0 'r["body"]["include"] == ["message.output_text.logprobs"]'

# "For"@0, " "@3, "sure"@4, "."@8
assert_state_contains "tokens:" dump anchors
assert_state_contains "text: For" dump anchors
assert_state_contains "lp: -0.75" dump anchors
assert_state_contains "pos: 4" dump anchors
assert_state_contains "pos: 8" dump anchors

mock_stop 1
pass
