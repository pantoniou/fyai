#!/bin/bash
# SPDX-License-Identifier: MIT
# Messages SSE streaming: text deltas over split chunks, usage split across
# message_start/message_delta, message_stop ends the stream (no [DONE]).
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start messages_stream.json

run_fyai --set api=messages -u "$MOCK_URL/v1/messages" -m mock-model \
	 --set display/stats=true "stream please"
assert_status 0
assert_stdout_contains "Streamed messages answer."

assert_request 0 'r["body"]["stream"] is True'
assert_request 0 '"stream_options" not in r["body"]'

# input from message_start, output from message_delta
assert_stderr_contains "input=14"
assert_stderr_contains "output=6"

# the canonical state records the assembled reply
assert_state_contains "Streamed messages answer." dump state

mock_stop 1
pass
