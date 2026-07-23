#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_stream_tool_overlap.json

run_fyai --set api=chat-completions --set tools=true \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "run while streaming"
assert_status 0
assert_stdout_contains "Streaming tool overlap verified."
[ -f streamed-tool.started ] ||
	fail "streamed tool did not execute"
[ ! -f stream-wait-failed ] ||
	fail "tool did not start before the response completed"

mock_stop 2
pass
