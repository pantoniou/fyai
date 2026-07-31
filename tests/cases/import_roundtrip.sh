#!/bin/bash
# SPDX-License-Identifier: MIT
# Import replays what export wrote: the same messages, the same turn splits and
# the same publish boundaries. Exporting the imported branch must reproduce the
# conversation byte for byte.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_tools_read_file.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	$'read sample.txt\n<!-- meta:yaml\nkind: turn\n-->'
assert_status 0

run_fyai export -o first.md
assert_status 0
mock_stop 2

# A second arena, so the import cannot be satisfied by state already present.
# The arena is found by walking up, so this directory needs its own marker or
# it resolves to the one the export came from.
export HOME="$TEST_DIR/home2"
mkdir -p "$HOME"
mkdir -p "$TEST_DIR/second"
cd "$TEST_DIR/second"
run_fyai init
assert_status 0
test -d .fyai || fail "the second arena was not created here"

run_fyai import -i ../first.md
assert_status 0

run_fyai export -o again.md
assert_status 0

# Compare the conversation. Configuration boundaries differ by construction:
# each arena carries its own bootstrap history, and the importing invocation
# resolves a model of its own, so only turns and messages are comparable.
conversation() {
	awk '/^<!-- meta:yaml$/ { inblk = 1; blk = $0 "\n"; next }
	     inblk { blk = blk $0 "\n"
		     if ($0 == "kind: config" || $0 == "kind: config-update")
			     drop = 1
		     if ($0 == "-->") { inblk = 0
				        if (!drop) printf "%s", blk
				        drop = 0 }
		     next }
	     !drop { print }' "$1"
}
conversation ../first.md | sed -n '/^kind: turn/,$p' > expected.txt
conversation again.md | sed -n '/^kind: turn/,$p' > actual.txt
diff -u expected.txt actual.txt || fail "the conversation did not round-trip"

# The reconstructed state must hold the real shapes, not just matching text.
grep -qx 'kind: tool_call' again.md || \
	fail "the tool call did not survive the round trip"
grep -q 'No such file or directory' again.md || \
	fail "the tool result did not survive the round trip"

# Prose that looks like a directive comes back as prose, not as structure.
grep -qx '<!-- x-meta:yaml' again.md || \
	fail "the escaped body was not restored"

# Turn and publish boundaries are replayed, not guessed.
test "$(grep -cx 'kind: turn' ../first.md)" \
	-eq "$(grep -cx 'kind: turn' again.md)" || \
	fail "the turn boundaries changed"
test "$(grep -cx 'kind: publish' again.md)" -ge 2 || \
	fail "the publish boundaries were not replayed"

# Standard input is the default, so export and import compose in a pipeline.
mkdir -p "$TEST_DIR/third"
cd "$TEST_DIR/third"
run_fyai init
assert_status 0
# run_fyai pins stdin to /dev/null, so drive the binary directly here.
set +e
"$FYAI_BIN" -k test-key --color off import <../first.md \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
FYAI_STATUS=$?
set -e
assert_status 0
run_fyai export -o piped.md
assert_status 0
conversation piped.md | sed -n '/^kind: turn/,$p' > piped.txt
diff -u ../second/expected.txt piped.txt || \
	fail "the piped import differs from the file import"

# A named branch is a destination; it must not absorb an existing conversation.
cd "$TEST_DIR/second"
run_fyai --branch main import -i ../first.md
assert_status_nonzero

pass
