#!/bin/bash
# SPDX-License-Identifier: MIT
# The native shell_call runs on a pseudo-terminal when shell/tty asks for one:
# the grammar has no field the model could ask with.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_builtin_shell_tty.json

run_fyai --set api=responses --set display/stream=false --set builtin_shell=true \
	 --set shell/tty=true --set shell/tty_cols=132 \
	 --set api_url="$MOCK_URL/v1/responses" -m mock-model "run the builtin shell"
assert_status 0
assert_stdout_contains "Builtin shell terminal round trip done."

# the command found a terminal, and it had the configured size
assert_request 1 'any("native-tty-ok" in o.get("stdout", "") and "132" in o["stdout"] and o["outcome"]["exit_code"] == 0 for i in r["body"]["input"] if i.get("type") == "shell_call_output" for o in i["output"])'
# a terminal has one stream, thus standard error stays empty
assert_request 1 'all(o.get("stderr", "") == "" for i in r["body"]["input"] if i.get("type") == "shell_call_output" for o in i["output"])'

mock_stop 2
pass
