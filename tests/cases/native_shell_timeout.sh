#!/bin/bash
# SPDX-License-Identifier: MIT
# A native shell call reaches its time limit.
#
# Use `timeout_ms` from the action instead of the configured default. Keep the
# standard native result shape: a sequence of {stdout, stderr, outcome}. A
# plain error string would discard the output and change the result schema.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start native_shell_timeout.json

# The configured default is much longer than the action limit. The turn
# completes quickly only when the action limit is active.
run_fyai --set api=responses --set display/stream=false \
	 --set builtin_shell=true --set shell/timeout_ms=60000 \
	 --set api_url="$MOCK_URL/v1/responses" \
	 -m mock-model "run a command that does not end"
assert_status 0
assert_stdout_contains "The command was stopped by its time limit."

# The result remains a structured shell_call_output. The outcome identifies
# the time limit instead of the termination signal.
assert_request 1 'any(i.get("type") == "shell_call_output" and i.get("call_id") == "sc_to_1" for i in r["body"]["input"])'
assert_request 1 'any(o["outcome"]["type"] == "timeout" and o["outcome"]["timeout_ms"] == 1500 for i in r["body"]["input"] if i.get("type") == "shell_call_output" for o in i["output"])'

# The model receives output that the command produced before the timeout.
assert_request 1 'any("shell-timeout-marker" in o.get("stdout", "") for i in r["body"]["input"] if i.get("type") == "shell_call_output" for o in i["output"])'

mock_stop 2
pass
