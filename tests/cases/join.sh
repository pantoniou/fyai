#!/bin/bash
# SPDX-License-Identifier: MIT
# Joining branches. A conversation is a list of messages, thus joining two of
# them is a question of order and not of reconciling edits. rebase puts our
# turns after theirs; merge interleaves the exchanges by when they happened.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start branch.json

cat > catalog.yaml <<EOF
models:
- name: foo
  capabilities: []
providers:
- name: mockprov
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: foo
    provider_model_id: bar
EOF
run_fyai catalog import catalog.yaml
assert_status 0

export MOCKPROV_API_KEY=mock-secret

run_bare() {
	set +e
	"$FYAI_BIN" --color off --set display/markdown=false \
		--set display/stream=false "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
}

# main gets A, then side is made and gets B, then main gets C. So the true
# order in time is A, B, C, but they are on two branches.
run_bare -m foo "AAA-first"
assert_status 0
run_fyai branch create side
assert_status 0
run_bare -b side -m foo "BBB-side"
assert_status 0
run_bare -m foo "CCC-second"
assert_status 0

# --- rebase: their turns, then ours ---------------------------------------
run_fyai rebase side
assert_status 0
assert_stdout_contains "rebased side into main"

# Everything is present, and main's own turns come last.
assert_state_contains "AAA-first" dump state
assert_state_contains "BBB-side" dump state
assert_state_contains "CCC-second" dump state

"$FYAI_BIN" dump state >"$TEST_DIR/rebased" 2>&1 || fail "dump after rebase"
grep -n "BBB-side" "$TEST_DIR/rebased" | head -1 | cut -d: -f1 >"$TEST_DIR/b"
grep -n "CCC-second" "$TEST_DIR/rebased" | head -1 | cut -d: -f1 >"$TEST_DIR/c"
[ "$(cat "$TEST_DIR/b")" -lt "$(cat "$TEST_DIR/c")" ] || \
	fail "rebase: side's turns must come before main's own"

# The join is recorded, with the branch it came from.
run_fyai list reflog
assert_status 0
assert_stdout_contains "rebase"
assert_stdout_contains "side"

# --- merge: chronological, per exchange -----------------------------------
# Go back to before the rebase and merge instead.
run_fyai reset "main@{1}"
assert_status 0
run_fyai merge side
assert_status 0
assert_stdout_contains "merged side into main"

"$FYAI_BIN" dump state >"$TEST_DIR/merged" 2>&1 || fail "dump after merge"
for t in AAA-first BBB-side CCC-second; do
	grep -qF "$t" "$TEST_DIR/merged" || fail "merge lost $t"
done
grep -n "AAA-first" "$TEST_DIR/merged" | head -1 | cut -d: -f1 >"$TEST_DIR/a"
grep -n "BBB-side" "$TEST_DIR/merged" | head -1 | cut -d: -f1 >"$TEST_DIR/b"
grep -n "CCC-second" "$TEST_DIR/merged" | head -1 | cut -d: -f1 >"$TEST_DIR/c"
[ "$(cat "$TEST_DIR/a")" -lt "$(cat "$TEST_DIR/b")" ] || fail "merge: A before B"
[ "$(cat "$TEST_DIR/b")" -lt "$(cat "$TEST_DIR/c")" ] || fail "merge: B before C"

# --- the source branch is untouched ---------------------------------------
# It inherited A when it was created, and keeps its own B without main's C.
assert_state_contains "AAA-first" -b side dump state
assert_state_contains "BBB-side" -b side dump state
assert_state_absent "CCC-second" -b side dump state

# --- refusals --------------------------------------------------------------
run_fyai merge main
assert_status 1
assert_stderr_contains "itself"

run_fyai merge nosuch
assert_status 1
assert_stderr_contains "no such branch"

# --- histories that never met --------------------------------------------
# A different system prompt gives the branch a genuinely different first turn.
# (With the same prompt the two share it: equal content is one stored value.)
# Selecting a not-yet-created branch starts it empty; unlike `branch create`,
# it deliberately has no start point to inherit.
run_fyai -b lone --set system_prompt=a-different-assistant
assert_status 0
run_bare -b lone -m foo "DDD-lone"
assert_status 0

run_fyai merge lone
assert_status 1
assert_stderr_contains "share no turn"

run_fyai merge --allow-unrelated lone
assert_status 0
assert_stdout_contains "unrelated"
assert_state_contains "DDD-lone" dump state
assert_state_contains "AAA-first" dump state

# One system prompt, not one per source joined.
"$FYAI_BIN" dump state >"$TEST_DIR/joined" 2>&1 || fail "dump after join"
[ "$(grep -c "role: system" "$TEST_DIR/joined")" -eq 1 ] || \
	fail "a joined conversation must keep exactly one system prompt"

mock_stop 4
pass
