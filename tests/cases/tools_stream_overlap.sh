#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_stream_tool_overlap.json

run_fyai --set api=chat-completions --set tools=true \
	--set api_url="$MOCK_URL/v1/chat/completions" \
	-m mock-model "run while streaming"
assert_status 0
assert_stdout_contains "Streaming tool overlap verified."
[ -f s ] ||
	fail "streamed tool did not execute"
[ ! -f stream-wait-failed ] ||
	fail "tool did not start before the response completed"

mock_stop 2
"$FYAI_BIN" transcript --raw --last 1 >"$TEST_DIR/transcript.out" ||
	fail "could not render streamed tool transcript"
"$PYTHON" - "$TEST_DIR/transcript.out" <<'EOF' ||
	fail "tool invocation was not separated from assistant prose"
import sys

data = open(sys.argv[1], "rb").read()
if b"shellshellshell\n\n**shell**" not in data:
    raise SystemExit("missing Markdown block boundary before tool call")
EOF
pass
