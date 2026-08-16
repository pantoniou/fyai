#!/bin/bash
# SPDX-License-Identifier: MIT
# A failed tool call keeps its chrome in the transcript. The live view marks the
# call and names the cause beside the label; history must replay both.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start display_parity_failure.json

run_fyai --set display/markdown=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set display/tool_detail=full \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "do things"
assert_status 0
# The cause sits beside the label on the title row, not only inside the tool
# result. Match label and cause together: either alone occurs elsewhere.
assert_stdout_contains "shell [fail on purpose] command exited with status 3"
assert_stdout_contains "read no-such-file.c No such file or directory"

cp "$TEST_DIR/stdout" "$TEST_DIR/live.txt"
# History repeats the user turn first; the rest is the same view.
"$FYAI_BIN" --color off history --last 1 | tail -n +3 > "$TEST_DIR/replay.txt"
grep -q "shell \[fail on purpose\] command exited with status 3" \
	"$TEST_DIR/replay.txt" ||
	fail "history replay dropped the shell failure cause"
grep -q "read no-such-file.c No such file or directory" \
	"$TEST_DIR/replay.txt" ||
	fail "history replay dropped the read failure cause"
diff -u "$TEST_DIR/live.txt" "$TEST_DIR/replay.txt" > "$TEST_DIR/parity.diff" ||
	{ cat "$TEST_DIR/parity.diff" >&2
	  fail "history replay diverged from the live render"; }

mock_stop 3
pass
