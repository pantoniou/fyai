#!/bin/bash
# SPDX-License-Identifier: MIT
# A sub-agent with completed tool work but no final model report is a failure,
# and its causal request diagnostic must reach the parent tool result.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_missing_report.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set tools=true --set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "delegate the missing-report test"
assert_status 0
assert_stdout_contains "Parent observed the agent failure."

assert_request 3 \
	'any(m.get("role") == "tool" and '\
'm.get("tool_call_id") == "call_agent_missing" and '\
'"tool error: sub-agent failed:" in m.get("content", "") and '\
'"mock child follow-up failed" in m.get("content", "") '\
'for m in r["body"]["messages"])'

# The completed child step remains recoverable on its branch.
run_fyai --branch main/agent:missing-report transcript --raw
assert_status 0
assert_stdout_contains "completed"

mock_stop 4
pass
