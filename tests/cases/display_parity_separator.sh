#!/bin/bash
# SPDX-License-Identifier: MIT
# A configured tool separator is presentation, not stored source. The renderer
# emits it on both paths, so replay must show it exactly as often as the live
# run did. Recording it as well drew it twice, which the empty default hid.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start display_parity.json

printf 'int main(void)\n{\n\tputs("old");\n\treturn 0;\n}\n' > hello.c

run_fyai --set display/markdown=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set display/tool_detail=full \
	 --set display/tool_separator=SEPMARK \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "do things"
assert_status 0
assert_stdout_contains "SEPMARK"

cp "$TEST_DIR/stdout" "$TEST_DIR/live.txt"
# History repeats the user turn first; the rest is the same view.
"$FYAI_BIN" --color off history --last 1 | tail -n +3 > "$TEST_DIR/replay.txt"
live=$(grep -c SEPMARK "$TEST_DIR/live.txt")
replay=$(grep -c SEPMARK "$TEST_DIR/replay.txt")
[ "$live" = "$replay" ] ||
	fail "replay drew $replay separators, the live run drew $live"
diff -u "$TEST_DIR/live.txt" "$TEST_DIR/replay.txt" > "$TEST_DIR/parity.diff" ||
	{ cat "$TEST_DIR/parity.diff" >&2
	  fail "history replay diverged from the live render"; }

mock_stop 5
pass
