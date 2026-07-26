#!/bin/bash
# SPDX-License-Identifier: MIT
# A delegated sub-agent must not write reasoning or statistics to the parent.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_reasoning_quiet.json

# Enable all optional diagnostic output.
run_fyai --set api=chat-completions --set display/stream=true \
	 --set tools=true --set thinking=true \
	 --set display/stats=true --set display/cache_info=true \
	 --set display/markdown=false \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate some thinking"
assert_status 0

# The delegation itself must still work.
assert_stdout_contains "Delegated and done."

# The sub-agent's reasoning must not reach the user, on either stream.
assert_stdout_not_contains "SUBAGENT-PRIVATE-THOUGHT"
assert_stderr_not_contains "SUBAGENT-PRIVATE-THOUGHT"

# Do not print the sub-agent report directly.
assert_stdout_not_contains "Sub-agent report."

# The parent still relayed it: the report reaches the model as the tool result.
assert_request 2 'any("Sub-agent report." in str(m.get("content", "")) for m in r["body"]["messages"])'

# The sub-agent really did stream reasoning - otherwise this passes vacuously.
assert_request 1 'r["body"]["stream"] is True'

mock_stop 3
pass
