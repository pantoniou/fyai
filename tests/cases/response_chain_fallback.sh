#!/bin/bash
# SPDX-License-Identifier: MIT
# A stale Responses previous_response_id falls back to the canonical arena
# history and establishes a fresh response chain.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start response_chain_fallback.json

run_fyai --set api=responses --set display/stream=false \
	--set response_chain=true --set api_url="$MOCK_URL/v1/responses" \
	-m mock-model --new "first turn"
assert_status 0
assert_stdout_contains "First answer."

run_fyai --set api=responses --set display/stream=false \
	--set response_chain=true --set api_url="$MOCK_URL/v1/responses" \
	-m mock-model "second turn"
assert_status 0
assert_stdout_contains "Recovered answer."

assert_request 1 'r["body"]["previous_response_id"] == "resp_first"'
assert_request 1 'len(r["body"]["input"]) == 1'
assert_request 2 '"previous_response_id" not in r["body"]'
assert_request 2 'any(x.get("role") == "user" and x.get("content") == "first turn" for x in r["body"]["input"])'
assert_request 2 'any(x.get("role") == "assistant" for x in r["body"]["input"])'
assert_request 2 'any(x.get("role") == "user" and x.get("content") == "second turn" for x in r["body"]["input"])'

mock_stop 3
pass
