#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a sub-agent runs under the tool sandbox: its runtime publishes to
# the project arena, which the sandbox denies, and the tool that it runs is
# confined in its own child.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

# The sandbox grants the system scratch trees whole: keep the project out of them.
FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
printf baseline > file
mock_start agent_sandbox.json

run_fyai --set sandbox=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	 "delegate an edit"
assert_status 0
assert_stdout_contains "Parent and sub-agent done."
[ "$(cat file)" = changed ] || fail 'the sub-agent did not run its tool'

# The tool of the sub-agent ran, and the sandbox kept it out of the arena.
assert_request 2 'any(m.get("role") == "tool" and "agent-tool-confined" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 2 'not any(m.get("role") == "tool" and "arena-writable" in m.get("content", "") for m in r["body"]["messages"])'

mock_stop 4
pass
