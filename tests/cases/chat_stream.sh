#!/bin/bash
# SPDX-License-Identifier: MIT
# Chat Completions SSE streaming: split chunks, finish_reason, usage chunk.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_stream.json

run_fyai --set api=chat-completions --set api_url="$MOCK_URL/v1/chat/completions" \
	 -m mock-model --set display/stats=true "stream please"
assert_status 0
assert_stdout_contains "Streaming hello from the mock."

assert_request 0 'r["body"]["stream"] is True'
assert_request 0 'r["body"]["stream_options"]["include_usage"] is True'

# usage from the final (choice-less) chunk must land in the run stats
assert_stderr_contains "input=20"
assert_stderr_contains "output=9"

mock_stop 1
pass
