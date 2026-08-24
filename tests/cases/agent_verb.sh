#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify one transient sub-agent verb invocation.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_verb.json

# Use an invalid MCP endpoint to verify that the sub-agent does not start MCP.
run_fyai --set api=chat-completions --set display/stream=false \
	 --set mcp/enabled=true --set "mcp/endpoint='$MOCK_URL/nonexistent-mcp'" \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 agent "print a greeting and report back"
assert_status 0
assert_stdout_contains "Sub-agent report: printed agent-was-here."

# The sub-agent runs under the built-in agent persona.
assert_request 0 'any(m.get("role") == "system" and "sub-agent" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 0 'any(m.get("role") == "system" and "Use apply_patch for manual code edits" in m.get("content", "") and "Do not create or edit files with exec_command" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 0 'any(m.get("role") == "system" and "Use ask_user" in m.get("content", "") and "cannot ask" not in m.get("content", "") for m in r["body"]["messages"])'

# Restricted toolset: builtins present, no nested agent. A question is kept:
# it goes up to whoever ran this sub-agent.
assert_request 0 'any(t["function"]["name"] == "exec_command" for t in r["body"]["tools"])'
assert_request 0 'any(t["function"]["name"] == "read_file" for t in r["body"]["tools"])'
assert_request 0 'any(t["function"]["name"] == "ask_user" for t in r["body"]["tools"])'
assert_request 0 'not any(t["function"]["name"] == "agent_input" for t in r["body"]["tools"])'
assert_request 0 'not any(t["function"]["name"] == "agent" for t in r["body"]["tools"])'
assert_request 0 'not any(t["function"]["name"].startswith("mcp__") for t in r["body"]["tools"])'

# ...and no MCP traffic was attempted at all.
assert_no_request 'r["path"].startswith("/nonexistent-mcp")'

# Second request carries the shell tool result back to the sub-agent's model.
assert_request 1 'any(m.get("role") == "tool" and m.get("tool_call_id") == "call_shell_1" for m in r["body"]["messages"])'

mock_stop 2

# Transient: nothing was published, so there is no persisted history.
run_fyai history --raw
assert_status 0
assert_stdout_not_contains "Sub-agent report"
pass
