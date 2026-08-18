#!/bin/bash
# SPDX-License-Identifier: MIT
# A command the shell tool execs inherits standard input, output and error and
# nothing else. Every other descriptor of the process belongs to fyai: the
# event loop, curl sockets, arena files, the JSON-RPC control channel of the
# tool child, pipes of sibling jobs. A command must not be able to read them,
# write into them, or hold them open - a write into the control channel alone
# would make the parent read a fragment and lose the frame around it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tool_child_channel.json

run_fyai --set api=chat-completions --set display/stream=false \
	 --set tools=true --set builtin_shell=false \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "probe"
assert_status 0
assert_stdout_contains "probe complete"

# The command ran and found nothing above standard error to dup.
assert_request 1 \
	'any(m.get("role") == "tool" and "NO_LEAKED_FDS" in m.get("content", "") '\
'for m in r["body"]["messages"])'

# The frames survived: a corrupted channel is reported, not silent.
grep -q "invalid stdio JSON" "$TEST_DIR/stderr" &&
	fail "the control channel was corrupted"

mock_stop 2
pass
