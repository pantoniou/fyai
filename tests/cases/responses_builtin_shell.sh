#!/bin/bash
# SPDX-License-Identifier: MIT
# Responses built-in shell tool: a native shell_call item executes and is
# answered with a shell_call_output carrying stdout/outcome.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_builtin_shell.json

run_fyai --responses --no-stream --builtin-shell \
	 -u "$MOCK_URL/v1/responses" -m mock-model "run the builtin shell"
assert_status 0
assert_stdout_contains "Builtin shell round trip done."

# first request advertises the built-in shell tool
assert_request 0 'any(t.get("type") == "shell" for t in r["body"]["tools"])'

# the output item type matches the call item type
assert_request 1 'any(i.get("type") == "shell_call_output" and i.get("call_id") == "sc_1" for i in r["body"]["input"])'
assert_request 1 'any("builtin-shell-ran" in o.get("stdout", "") and o["outcome"]["exit_code"] == 0 for i in r["body"]["input"] if i.get("type") == "shell_call_output" for o in i["output"])'

mock_stop 2
pass
