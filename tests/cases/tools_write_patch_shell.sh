#!/bin/bash
# SPDX-License-Identifier: MIT
# A three-call tool turn: write_file, apply_patch, shell - all must execute
# and their results round-trip in the follow-up request.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tools_write_patch_shell.json

run_fyai --set display/markdown=true --set api=chat-completions --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "do the three things"
assert_status 0
assert_stdout_contains "All three tools executed."
# Off a terminal every tool renders through the transcript path, so the call
# lands on stdout in the same shape history replays: a label, then the command
# in a marked, frameless block.
assert_stdout_contains "  shell"
assert_stdout_contains "⎿  echo shell-ran-ok"
assert_stdout_contains "shell-ran-ok"

assert_file_content out.txt "written by mock"
assert_file_content patched.txt "patched line"

assert_request 1 'any(m.get("tool_call_id") == "call_write_1" and m.get("content") == "ok" for m in r["body"]["messages"])'
assert_request 1 'any(m.get("tool_call_id") == "call_patch_1" and m.get("content", "").startswith("ok") for m in r["body"]["messages"])'
assert_request 1 'any(m.get("tool_call_id") == "call_shell_1" and "shell-ran-ok" in m.get("content", "") for m in r["body"]["messages"])'

mock_stop 2
pass
