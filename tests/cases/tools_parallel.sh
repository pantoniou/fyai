#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tools_parallel.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set tools=true \
	--set parallel_tool_calls_prompt="Run independent calls together." \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "run both independent commands"
assert_status 0
assert_stdout_contains "parallel tools completed"
[ -f parallel.marker ] || fail "parallel marker was not created"
[ -f first.done ] || fail "the first tool did not overlap the second"

assert_request 0 'r["body"]["parallel_tool_calls"] is True'
assert_request 0 \
	'"Run independent calls together." in '\
'r["body"]["messages"][0]["content"]'
assert_request 1 \
	'any(m.get("tool_call_id") == "call_wait" and '\
'"first-ok" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 1 \
	'any(m.get("tool_call_id") == "call_mark" and '\
'"second-ok" in m.get("content", "") for m in r["body"]["messages"])'

mock_stop 2
pass
