#!/bin/bash
# SPDX-License-Identifier: MIT
# The tool loop limit keeps the completed steps and reports the cause.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_tools_read_file.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set max_tool_iterations=1 \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "read missing.py"
assert_stderr_contains "tool loop reached its limit"

mock_stop 1

# the completed tool step is committed, not forgotten
run_fyai history --raw
assert_status 0
assert_stdout_contains "tool error:"
pass
