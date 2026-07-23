#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_builtin_shell_parallel.json

run_fyai --set api=responses --set display/stream=false \
	--set builtin_shell=true \
	--set api_url="$MOCK_URL/v1/responses" \
	-m mock-model "run native calls together"
assert_status 0
assert_stdout_contains "Native parallel tools done."
[ -f native.marker ] || fail "native marker was not created"
[ -f native.first ] || fail "native shell calls did not overlap"

assert_request 1 \
	'all(any(i.get("type") == "shell_call_output" and '\
'i.get("call_id") == call for i in r["body"]["input"]) for call in '\
'("call_native_wait", "call_native_mark"))'
assert_request 1 \
	'any("native-first" in o.get("stdout", "") for i in '\
'r["body"]["input"] if i.get("type") == "shell_call_output" and '\
'i.get("call_id") == "call_native_wait" '\
'for o in i["output"])'
assert_request 1 \
	'any("native-second" in o.get("stdout", "") for i in '\
'r["body"]["input"] if i.get("type") == "shell_call_output" and '\
'i.get("call_id") == "call_native_mark" '\
'for o in i["output"])'

mock_stop 2
pass
