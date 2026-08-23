#!/bin/bash
# SPDX-License-Identifier: MIT
# The shell tool is declared as `exec_command`, but `shell` is the name it had
# and the name in the stored calls of an older arena. A call under either name
# reaches the same tool, and the verb takes either name too.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start exec_command_legacy_name.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set "api_url=$MOCK_URL/v1/chat/completions" \
	 -m mock-model "run the legacy name"
assert_status 0
assert_stdout_contains "Legacy name accepted."

# The declaration carries the new name only.
assert_request 0 'any(t["function"]["name"] == "exec_command" for t in r["body"]["tools"])'
assert_request 0 'not any(t["function"]["name"] == "shell" for t in r["body"]["tools"])'
# The call under the old name ran, and its result went back to the provider.
assert_request 1 'any("legacy-name-ran" in m.get("content", "") for m in r["body"]["messages"] if m.get("tool_call_id") == "call_legacy")'

mock_stop 2

# The verb takes either name.
run_fyai tool exec_command '{"command":"echo verb-new-name"}'
assert_status 0
assert_stdout_contains "verb-new-name"
run_fyai tool shell '{"command":"echo verb-old-name"}'
assert_status 0
assert_stdout_contains "verb-old-name"
pass
