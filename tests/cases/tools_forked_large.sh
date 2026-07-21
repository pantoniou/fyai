#!/bin/bash
# SPDX-License-Identifier: MIT
# A forked tool whose result is far larger than the pipe buffer must round-trip
# whole.
#
# This is the deadlock case for the fork-per-tool transport: a result past the
# ~64 KiB pipe capacity blocks the child in write() until the parent drains it,
# so a collector that waited for the child to exit before reading would hang
# forever, and one that stopped reading at child exit would truncate. The
# collector therefore watches the result pipe and the child together and
# finishes on pipe EOF, not on exit.
#
# ~40k lines is several hundred KiB - many pipe-buffer refills, so the child
# blocks and resumes repeatedly during the transfer.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tools_forked_large.json

run_fyai --set sandbox=true --set api=chat-completions --set display/stream=false \
	--set tools=true --set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "produce a large result"
assert_status 0
assert_stdout_contains "large result received"

# The whole result was piped back: both ends of the range survived, and the
# tool message is at least as large as the output itself.
assert_request 1 'any(m.get("tool_call_id") == "call_shell_big" and "\n2\n3\n" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 1 'any(m.get("tool_call_id") == "call_shell_big" and "40000" in m.get("content", "") for m in r["body"]["messages"])'
assert_request 1 'any(m.get("tool_call_id") == "call_shell_big" and len(m.get("content", "")) > 200000 for m in r["body"]["messages"])'

mock_stop 2
pass
