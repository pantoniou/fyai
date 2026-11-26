#!/bin/bash
# SPDX-License-Identifier: MIT
# An idle session follows its branch: a turn that another invocation stores
# on the branch shows in the transcript of the session while it waits, and
# its next request carries that turn.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start branch_follow.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1

SYNC="$TEST_DIR/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/stored"

# The session stores its first turn, then waits at the prompt while another
# invocation adds a turn to its branch. A command runs only after the turn
# before it is stored, so its output says the turn is stored.
# The transcript view of a full-screen page shows the turns it took; an inline
# screen keeps the rows it wrote.
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main \
    --set display/screen=fullscreen -i <<PTY &
steps:
  - type: Alpha from A
  - wait-screen: "Answer alpha."
    timeout: session
  - type: /branch describe main Following ready
  - wait-screen: "described branch main"
    timeout: session
  - touch: "$SYNC/stored"
    timeout: session
  - wait-screen: "Answer bravo."
    timeout: session
  - type: Charlie from A
  - wait-screen: "Answer charlie."
    timeout: session
  - type: /exit
PTY
session=$!
# A session that stops before it says so is a failure, not a hang.
(read -r _ <"$SYNC/stored") &
reader=$!
wait -n "$reader" "$session" || true
if ! kill -0 "$reader" 2>/dev/null; then :; else
	kill "$reader" 2>/dev/null || true
	fail "the session stopped before it stored its turn: $(tail -c 2000 "$TEST_DIR/pty.out")"
fi
"$FYAI_BIN" -b main "Bravo from B" >"$TEST_DIR/b.out" 2>&1 ||
	fail "the second invocation failed: $(cat "$TEST_DIR/b.out")"
wait "$session" || fail "the session did not follow its branch: $(tail -c 2000 "$TEST_DIR/pty.out")"
mock_stop 3

# The next request of the session holds the turn of the other invocation.
assert_request 2 '[m["content"] for m in r["body"]["messages"] if m["role"] != "system"][-5:] == ["Alpha from A", "Answer alpha.", "Bravo from B", "Answer bravo.", "Charlie from A"]'
# The branch holds the three exchanges in order.
"$FYAI_BIN" -b main export -o saved.md >/dev/null 2>&1 || fail "no export"
"$PYTHON" - saved.md <<'PY' || fail "the branch does not hold the three exchanges in order"
import sys
t = open(sys.argv[1]).read()
at = [t.find(w) for w in ("Alpha from A", "Bravo from B", "Charlie from A",
                          "Answer charlie.")]
sys.exit(0 if -1 not in at and at == sorted(at) else 1)
PY
pass
