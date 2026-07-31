#!/bin/bash
# SPDX-License-Identifier: MIT
# Replay re-issues the user turns against the environment as it is now. The
# stored assistant turns are not restored and the tool calls are not replayed,
# because the model runs again and re-derives them against current state.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic_twice.json

run_fyai --set api=chat-completions --set display/stream=false \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	"what did you find"
assert_status 0

# The branch holds one user turn and the answer to it.
run_fyai export -o before.md
assert_status 0
grep -q 'what did you find' before.md || fail "the user turn was not stored"

run_fyai --set display/stream=false -m mock-model replay
assert_status 0

run_fyai export -o after.md
assert_status 0

# The question is asked again, so it is still there.
grep -q 'what did you find' after.md || fail "replay dropped the user turn"

# The replay issued a fresh request carrying that same user turn.
assert_request 1 'any("what did you find" in str(m) for m in r["body"]["messages"])'

# The chain is restarted, not appended to. The export still shows the earlier
# conversation, because it walks the whole ref log and that history really
# happened; the current chain begins at the last system message.
start=$(grep -n '^role: system' after.md | tail -1 | cut -d: -f1)
tail -n "+$start" after.md > current.txt
test "$(grep -c 'what did you find' current.txt)" -eq 1 || \
	fail "the replayed chain does not hold exactly one user turn"
test "$(grep -c '^role: system' after.md)" -eq 2 || \
	fail "replay did not restart the chain with a fresh system turn"

mock_stop 2
pass
