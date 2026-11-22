#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a pre-checkpoint view record is protected by its runtime lock:
# a second start refuses to drop the view while the lock is held, and drops it
# once the lock is released.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
fyai_test_scratch
fyai_test_on_exit 'chmod -R u+rwx "$TEST_DIR"/.fyai/views 2>/dev/null || true'
mkfifo "$TEST_SCRATCH/ready"
mock_start chat_basic.json
export OPENAI_API_KEY=test-key

run_fyai config set view/ignore '["home","tmp","port","served","mock.log","requests.jsonl","pty1.out","stdout","stderr","out","err","config.yaml","recordings","pty-script.*"]'
run_fyai config set display/stream false
"$FYAI_BIN" branch create s1 >/dev/null 2>&1 || fail 'no branch'
for kv in "api chat-completions" "model mock-model" \
	  "api_url $MOCK_URL/v1/chat/completions" \
	  "view/isolate_session true" "durable true"; do
	set -- $kv
	run_fyai -b s1 config set "$1" "$2"
	assert_status 0
done

# Start a session and kill it before any request so no checkpoint exists.
(pty_run "$TEST_DIR/pty1.out" "$FYAI_BIN" -b s1 -i <<PTY
timeout: 60
steps:
  - wait-screen: "❯"
    timeout: session
  - touch: $TEST_SCRATCH/ready
    timeout: session
  - wait-screen: NEVER-ON-SCREEN
    timeout: session
PTY
) >/dev/null 2>&1 &
driver=$!
read -r _ <"$TEST_SCRATCH/ready" || true

tree=$(ps -eo pid=,args= | grep -F -- "$TEST_DIR" | grep -v grep |
       awk -v mock="$MOCK_PID" '$1 != mock {print $1}')
all=$tree
while [ -n "$tree" ]; do
	next=
	for p in $tree; do next="$next $(pgrep -P "$p" || true)"; done
	tree=$(echo $next)
	all="$all $tree"
done
kill -9 $all 2>/dev/null || true
wait "$driver" 2>/dev/null || true

lock=$(find "$TEST_DIR/.fyai" -path '*views/view-*' -name lock | head -1)
[ -n "$lock" ] || fail 'the view has no lock file'

# Simulate an active session holding the view lock: must refuse to drop it.
exec 9>"$lock"
flock -x -n 9 || fail 'the lock of the view is held already'
run_fyai -b s1 "hello"
[ "$FYAI_STATUS" -ne 0 ] || fail 'a view in use was dropped'
grep -qF "is in use by a session" "$TEST_DIR/stderr" ||
	fail "no refusal: $(cat "$TEST_DIR/stderr")"
! grep -qF "was lost before its first checkpoint" "$TEST_DIR/stderr" ||
	fail 'the view in use was reported dropped'

# Release the lock: the stale pre-checkpoint record is dropped and session runs.
exec 9>&-
run_fyai -b s1 "hello"
assert_status 0
grep -qF "was lost before its first checkpoint" "$TEST_DIR/stderr" ||
	fail "the lost view was not dropped: $(cat "$TEST_DIR/stderr")"
mock_stop
pass
