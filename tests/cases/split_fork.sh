#!/bin/bash
# SPDX-License-Identifier: MIT
# Two sessions of one branch that both commit (the state of /split): with
# branch/on_conflict fork the one that commits second moves to a new session
# branch that starts where it started, and the branch of the first is whole.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start split_fork.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}'
	    --set branch/on_conflict=fork)
run_fyai "${set_common[@]}" -b main "first"
assert_status 0

# Both runs open the same root and meet at the gate before they publish.
gate="$TEST_DIR/gate"
mkdir -p "$gate"
set +e
FYAI_TEST_BRANCH_CAS_GATE="$gate" FYAI_TEST_BRANCH_CAS_PEERS=2 \
	"$FYAI_BIN" --color off "${set_common[@]}" -b main "left" \
	>"$TEST_DIR/left.out" 2>"$TEST_DIR/left.err" &
left_pid=$!
FYAI_TEST_BRANCH_CAS_GATE="$gate" FYAI_TEST_BRANCH_CAS_PEERS=2 \
	"$FYAI_BIN" --color off "${set_common[@]}" -b main "right" \
	>"$TEST_DIR/right.out" 2>"$TEST_DIR/right.err" &
right_pid=$!
wait "$left_pid"
left_status=$?
wait "$right_pid"
right_status=$?
set -e
[ "$left_status" -eq 0 ] && [ "$right_status" -eq 0 ] ||
	fail "a run failed: $(cat "$TEST_DIR"/left.err "$TEST_DIR"/right.err)"
grep -qF "this session goes on in 'session/" "$TEST_DIR"/left.err \
	"$TEST_DIR"/right.err ||
	fail "no run moved to a new branch: $(cat "$TEST_DIR"/*.err)"

run_fyai branch list
assert_stdout_contains "session/"
assert_stdout_contains "main"
pass
