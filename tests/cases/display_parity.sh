#!/bin/bash
# SPDX-License-Identifier: MIT
# What a turn shows while it runs and what history replays are the same view.
# Every tool renders through one path, so the two must agree byte for byte.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start display_parity.json

printf 'int main(void)\n{\n\tputs("old");\n\treturn 0;\n}\n' > hello.c

run_fyai --set display/markdown=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set display/tool_detail=full \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "do things"
assert_status 0
assert_stdout_contains "⎿  echo one; echo two"

# The transcript is standard output on both sides. Standard error carries only
# diagnostics, and under a sanitizer it also carries library reports.
cp "$TEST_DIR/stdout" "$TEST_DIR/live.txt"
# History repeats the user turn first; the rest is the same view.
"$FYAI_BIN" --color off history --last 1 | tail -n +3 > "$TEST_DIR/replay.txt"
diff -u "$TEST_DIR/live.txt" "$TEST_DIR/replay.txt" > "$TEST_DIR/parity.diff" ||
	{ cat "$TEST_DIR/parity.diff" >&2
	  fail "history replay diverged from the live render"; }

mock_stop 5
pass
