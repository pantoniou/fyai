#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify one delegated sub-agent tool call.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_tool.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate a greeting to a sub-agent"
assert_status 0
assert_stdout_contains "Delegated and done."

# Top-level model advertises the agent tool.
assert_request 0 'any(t["function"]["name"] == "agent" for t in r["body"]["tools"])'

# A forked sub-agent request runs under the built-in persona with no agent tool.
# The default is `context: fork`, thus the sub-agent keeps the system turn of
# the conversation it came from and the persona arrives as an instruction
# message before the task.
assert_request 1 'any("sub-agent" in str(m.get("content", "")) for m in r["body"]["messages"])'
assert_request 1 'not any(t["function"]["name"] == "agent" for t in r["body"]["tools"])'

# The parent's final request replays its agent tool call and carries the
# sub-agent report back as the tool result.
assert_request 3 'any(m.get("role") == "assistant" and m.get("tool_calls") and m["tool_calls"][0]["function"]["name"] == "agent" for m in r["body"]["messages"])'
assert_request 3 'any(m.get("role") == "tool" and m.get("tool_call_id") == "call_agent_1" and "SUBAGENT_REPORT" in m.get("content", "") for m in r["body"]["messages"])'

mock_stop 4
pass
