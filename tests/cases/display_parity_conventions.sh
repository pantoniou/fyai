#!/bin/bash
# SPDX-License-Identifier: MIT
# One manager applies the separation conventions, thus a live run and a replay
# draw the same view for each setting. A convention that reaches one path only
# draws a row on one side.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

printf 'int main(void)\n{\n\tputs("old");\n\treturn 0;\n}\n' > hello.c

# Each case is one set of conventions applied to the same conversation.
run_case()
{
	local name="$1"; shift
	mock_start display_parity.json
	run_fyai --set display/markdown=true --set api=chat-completions \
		 --set display/stream=false --set tools=true \
		 --set display/tool_detail=full \
		 "$@" \
		 --set api_url="$MOCK_URL/v1/chat/completions" \
		 -m mock-model "do things"
	assert_status 0
	cp "$TEST_DIR/stdout" "$TEST_DIR/live-$name.txt"
	# History repeats the user turn. Drop the quoted line and the
	# separation under it, whose height these cases change.
	"$FYAI_BIN" --color off history --last 1 |
		sed '1{/^> /d;}' | sed '/./,$!d' \
		> "$TEST_DIR/replay-$name.txt"
	diff -u "$TEST_DIR/live-$name.txt" "$TEST_DIR/replay-$name.txt" \
		> "$TEST_DIR/parity-$name.diff" ||
		{ cat "$TEST_DIR/parity-$name.diff" >&2
		  fail "$name: the replay diverged from the live render"; }
	mock_stop 5
	# Each case starts from an empty arena so history holds one exchange.
	rm -rf "$HOME/.fyai"
}

run_case default
run_case no-fence --set display/tool_group_fence=0
run_case wide-fence --set display/tool_group_fence=2
run_case no-card-fence --set display/user_card_fence=0
run_case tool-sep --set display/tool_separator=SEPMARK
run_case rule --set 'display/turn_separator="***"'
run_case brief --set display/tool_detail=brief

# A setting must not make one path draw a row the other does not. Without a
# terminal, the separation of an assistant document comes from its stored
# Markdown. These cases thus compare the two paths and do not measure the
# height of the fence, which the live terminal medium owns.

pass
