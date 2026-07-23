#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tools_parallel.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set tools=true --set parallel_tool_calls=false \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "run both commands serially"
assert_status 0
[ -f parallel.marker ] || fail "the second tool did not run"
[ ! -f first.done ] || fail "disabled tools unexpectedly overlapped"

assert_request 0 '"parallel_tool_calls" not in r["body"]'
assert_request 0 \
	'"Independent tool calls may be issued together" not in '\
'r["body"]["messages"][0]["content"]'

mock_stop 2
pass
