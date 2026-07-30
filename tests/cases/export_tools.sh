#!/bin/bash
# SPDX-License-Identifier: MIT
# Textual export: a tool call and its result are separate messages and land in
# different stored turns, yet the export joins them into one directive, so
# nothing in the file names a call by number or by position.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_tools_read_file.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	"read sample.txt"
assert_status 0

run_fyai export -o saved.md
assert_status 0

grep -qx 'kind: tool_call' saved.md || fail "missing the tool call"
grep -qx 'tool: read_file' saved.md || fail "missing the tool name"
grep -q 'No such file or directory' saved.md || \
	fail "the call did not carry its result"

# The result is a key of the call, never a turn and never its own directive.
! grep -qx 'role: tool' saved.md || \
	fail "the tool result was exported as a plain turn"
! grep -qx 'kind: tool_result' saved.md || \
	fail "the tool result was exported as a separate directive"

# Nothing names a call: no ordinal to renumber under a merge.
! grep -qE '^call: [0-9]+' saved.md || \
	fail "the export still uses call ordinals"

# One call, and the result sits inside it.
test "$(grep -cx 'kind: tool_call' saved.md)" -eq 1 || \
	fail "expected one tool call"
block=$(awk '/^kind: tool_call$/ { inblk = 1 } inblk { print } /^-->$/ { inblk = 0 }' \
	saved.md)
grep -q 'No such file or directory' <<<"$block" || \
	fail "the result is not inside the call directive"

mock_stop 2
pass
