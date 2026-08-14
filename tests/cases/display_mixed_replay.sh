#!/bin/bash
# SPDX-License-Identifier: MIT
# An exchange with no stored display output - an imported conversation, an old
# arena - must not decide how the other exchanges render. Each one replays what
# its own live view showed.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start display_parity.json

printf 'int main(void)\n{\n\tputs("old");\n\treturn 0;\n}\n' > hello.c

# An imported exchange carries messages only, so it has nothing to replay.
"$FYAI_BIN" --color off import -i "$TESTS_DIR/data/imported_conversation.md" \
	>/dev/null 2>&1 || fail "import failed"

run_fyai --set display/markdown=true --set api=chat-completions \
	 --set display/stream=false --set tools=true \
	 --set display/tool_detail=full \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "do things again"
assert_status 0

"$FYAI_BIN" --color off transcript >"$TEST_DIR/all.out" 2>&1 ||
	fail "transcript failed"
grep -qF "do things" "$TEST_DIR/all.out" ||
	fail "the imported exchange is missing"
grep -qF "do things again" "$TEST_DIR/all.out" ||
	fail "the recorded exchange is missing"

# The recorded exchange replays its stored view: the command block is marked
# and frameless. The imported one has nothing to replay, so it keeps the
# reconstructed shape - and only it does.
marked=$(grep -cF "⎿  echo one; echo two" "$TEST_DIR/all.out" || true)
framed=$(grep -cF "── sh ─" "$TEST_DIR/all.out" || true)
[ "$marked" = "1" ] ||
	fail "recorded exchange lost its marked command block (marked=$marked)"
[ "$framed" = "1" ] ||
	fail "reconstruction leaked into the recorded exchange (framed=$framed)"

mock_stop 5
pass
