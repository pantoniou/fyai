#!/bin/bash
# SPDX-License-Identifier: MIT
# With the sandbox enabled on Linux, each built-in tool call runs in its own
# forked, sandboxed child and its result is piped back to the parent. This
# reuses the write_file/apply_patch/shell scenario and asserts the same
# outcomes, proving the fork-per-tool transport round-trips. (Where Landlock is
# absent the child still forks and pipes; only the confinement is a no-op.)
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tools_write_patch_shell.json

run_fyai --set sandbox=true --set api=chat-completions --set display/stream=false -t \
	 -u "$MOCK_URL/v1/chat/completions" -m mock-model "do the three things"
assert_status 0
assert_stdout_contains "All three tools executed."

# The forked children wrote these into the (shared) working directory.
assert_file_content out.txt "written by mock"
assert_file_content patched.txt "patched line"

# Each tool result was piped back and folded into the follow-up request.
assert_request 1 'any(m.get("tool_call_id") == "call_write_1" and m.get("content") == "ok" for m in r["body"]["messages"])'
assert_request 1 'any(m.get("tool_call_id") == "call_patch_1" and m.get("content", "").startswith("ok") for m in r["body"]["messages"])'
assert_request 1 'any(m.get("tool_call_id") == "call_shell_1" and "shell-ran-ok" in m.get("content", "") for m in r["body"]["messages"])'

mock_stop 2
pass
