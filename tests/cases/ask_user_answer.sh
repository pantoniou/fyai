#!/bin/bash
# SPDX-License-Identifier: MIT
# ask_user tool: --answer feeds the canned reply; without one on a
# non-interactive stdin the run aborts with the documented error.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ask_user.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 --answer yes "ask me something"
assert_status 0
assert_stdout_contains "User said yes, proceeding."
assert_request 1 'any(m.get("role") == "tool" and m.get("tool_call_id") == "call_ask_1" and m.get("content") == "yes" for m in r["body"]["messages"])'
mock_stop 2

# negative: no --answer, stdin is not a tty -> the run flags an abort
mock_start ask_user.json
run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "ask me something"
assert_stderr_contains "an answer is expected but none is available"
mock_stop 1

pass
