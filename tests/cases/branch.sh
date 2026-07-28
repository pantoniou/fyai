#!/bin/bash
# SPDX-License-Identifier: MIT
# Branching: a branch is an independent line of work with its own conversation
# and its own configuration. Covers --branch/-b, $FYAI_BRANCH, the branch and
# checkout verbs, forking at a symbolic start point, and the refusal of
# numeric references.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start branch.json

cat > catalog.yaml <<EOF
models:
- name: foo
  capabilities: []
- name: baz
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
  - canonical_id: baz
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

# A fresh arena starts on main.
run_fyai branch
assert_status 0
assert_stdout_contains "main"

# --- per-branch configuration -------------------------------------------
run_fyai --set model=foo
assert_status 0
run_fyai -b exp --set model=baz
assert_status 0

# Each branch keeps its own model; neither leaks into the other.
run_fyai config get model
assert_status 0
assert_stdout_contains "foo"
run_fyai -b exp config get model
assert_status 0
assert_stdout_contains "baz"

# $FYAI_BRANCH selects the same branch as -b.
FYAI_BRANCH=exp run_fyai config get model
assert_status 0
assert_stdout_contains "baz"

# --branch selects a branch for one invocation only: HEAD does not move.
run_fyai branch
assert_status 0
assert_stdout_contains "main"
run_fyai config get model
assert_status 0
assert_stdout_contains "foo"

# --- conversations are per branch ---------------------------------------
run_bare -m foo "first prompt"
assert_status 0
assert_stdout_contains "Reply one."

# exp was never used for a turn, so it is still empty.
assert_state_absent "first prompt" -b exp dump state

# main carries the turn.
assert_state_contains "first prompt" dump state

# --- forking at a symbolic start point ----------------------------------
run_bare -m foo "second prompt"
assert_status 0

# A turn is one message append, as `list turns` shows: the second exchange is
# a user turn plus an assistant turn, so main~2 forks before it.
run_fyai branch create fork main~2
assert_status 0
assert_state_contains "first prompt" -b fork dump state
assert_state_absent "second prompt" -b fork dump state

# The branch's own ref log is addressable, and a start point stands for the
# whole state there - the conversation and the settings - as it does in git.
run_fyai --set model=baz
assert_status 0
run_fyai branch create old "main@{1}"
assert_status 0
run_fyai -b old config get model
assert_status 0
assert_stdout_contains "foo"

# Without a start point the current branch is the start point.
run_fyai branch create sibling
assert_status 0
run_fyai -b sibling config get model
assert_status 0
assert_stdout_contains "baz"
assert_state_contains "first prompt" -b sibling dump state
assert_state_contains "second prompt" -b sibling dump state

# `checkout -b` without an explicit start inherits the same whole state.
run_fyai checkout -b checkout-copy
assert_status 0
assert_state_contains "first prompt" dump state
assert_state_contains "second prompt" dump state
run_fyai checkout main
assert_status 0

# `checkout -b <new> <start>` is one step, as git spells it.
run_fyai checkout -b recovered "main@{1}"
assert_status 0
assert_stdout_contains "switched to branch recovered"
run_fyai config get model
assert_status 0
assert_stdout_contains "baz"
run_fyai checkout main
assert_status 0

# A start point without -b is refused rather than silently ignored.
run_fyai checkout main "main@{1}"
assert_status 1
assert_stderr_contains "needs -b"

# --- numeric references are refused -------------------------------------
run_fyai branch create bad 7f3a1200
assert_status 1
assert_stderr_contains "numeric"

# --- checkout moves HEAD durably -----------------------------------------
run_fyai checkout exp
assert_status 0
assert_stdout_contains "switched to branch exp"

run_fyai config get model
assert_status 0
assert_stdout_contains "baz"

# --- delete and rename ---------------------------------------------------
run_fyai branch delete fork --force
assert_status 0
run_fyai branch
assert_status 0
assert_stdout_not_contains "fork"

run_fyai branch rename old ancient
assert_status 0
run_fyai branch
assert_status 0
assert_stdout_contains "ancient"

# Deleting a parent reparents its children.
run_fyai branch create delete-parent "exp"
assert_status 0
run_fyai branch create delete-parent/child main
assert_status 0
run_fyai branch delete delete-parent --force
assert_status 0
run_fyai branch show child
assert_status 0
assert_stdout_contains "child"
run_fyai branch show delete-parent/child
assert_status 1

# Reparenting must not overwrite an existing branch.
run_fyai branch create collision
assert_status 0
run_fyai branch create delete-collision
assert_status 0
run_fyai branch create delete-collision/collision
assert_status 0
run_fyai branch delete delete-collision --force
assert_status 1
assert_stderr_contains "collision"

# Renaming a hierarchy must not overwrite an existing destination child.
run_fyai branch create rename-source
assert_status 0
run_fyai branch create rename-source/child
assert_status 0
run_fyai branch create rename-dest/child
assert_status 0
run_fyai branch rename rename-source rename-dest
assert_status 1
assert_stderr_contains "rename-dest/child"

# --- concurrent foreign-branch mutations ---------------------------------
# Delay both publishers after they have opened the same root. One must lose
# the CAS and reapply its disjoint branch-table delta to the surviving root.
run_cas_pair() {
	tag=$1
	shift
	set +e
	FYAI_TEST_BRANCH_CAS_DELAY_MS=300 "$FYAI_BIN" --color off \
		"$@" >"$TEST_DIR/$tag.left.out" 2>"$TEST_DIR/$tag.left.err" &
	left_pid=$!
	FYAI_TEST_BRANCH_CAS_DELAY_MS=300 "$FYAI_BIN" --color off \
		"${CAS_RIGHT[@]}" >"$TEST_DIR/$tag.right.out" \
		2>"$TEST_DIR/$tag.right.err" &
	right_pid=$!
	wait "$left_pid"
	left_status=$?
	wait "$right_pid"
	right_status=$?
	set -e
	[ "$left_status" -eq 0 ] || fail "$tag left operation failed"
	[ "$right_status" -eq 0 ] || fail "$tag right operation failed"
	grep -qF "reapplied branch-table changes" \
		"$TEST_DIR/$tag.left.err" "$TEST_DIR/$tag.right.err" || \
		fail "$tag did not exercise CAS reconciliation"
}

# A foreign branch-table operation must not append a synthetic config entry
# to the active branch's own reflog.
"$FYAI_BIN" root print main >"$TEST_DIR/main-before" 2>/dev/null
run_fyai branch create no-reflog
assert_status 0
"$FYAI_BIN" root print main >"$TEST_DIR/main-after" 2>/dev/null
cmp -s "$TEST_DIR/main-before" "$TEST_DIR/main-after" || \
	fail "foreign branch creation changed main's reflog"

CAS_RIGHT=(branch create cas-b)
run_cas_pair create branch create cas-a
run_fyai branch
assert_status 0
assert_stdout_contains "cas-a"
assert_stdout_contains "cas-b"

CAS_RIGHT=(branch describe cas-b "right description")
run_cas_pair describe branch describe cas-a "left description"
run_fyai branch show cas-a
assert_status 0
assert_stdout_contains "left description"
run_fyai branch show cas-b
assert_status 0
assert_stdout_contains "right description"

CAS_RIGHT=(branch rename cas-b renamed-b)
run_cas_pair rename branch rename cas-a renamed-a
run_fyai branch
assert_status 0
assert_stdout_contains "renamed-a"
assert_stdout_contains "renamed-b"
assert_stdout_not_contains "cas-a"
assert_stdout_not_contains "cas-b"

CAS_RIGHT=(branch delete renamed-b --force)
run_cas_pair delete branch delete renamed-a --force
run_fyai branch
assert_status 0
assert_stdout_not_contains "renamed-a"
assert_stdout_not_contains "renamed-b"

# Two writers changing the same branch are a real conflict: exactly one wins
# and the other must fail explicitly rather than claim a dropped update.
run_fyai branch create cas-same
assert_status 0
set +e
FYAI_TEST_BRANCH_CAS_DELAY_MS=300 "$FYAI_BIN" --color off \
	branch describe cas-same "same left" \
	>"$TEST_DIR/same.left.out" 2>"$TEST_DIR/same.left.err" &
same_left_pid=$!
FYAI_TEST_BRANCH_CAS_DELAY_MS=300 "$FYAI_BIN" --color off \
	branch describe cas-same "same right" \
	>"$TEST_DIR/same.right.out" 2>"$TEST_DIR/same.right.err" &
same_right_pid=$!
wait "$same_left_pid"
same_left_status=$?
wait "$same_right_pid"
same_right_status=$?
set -e
[ $((same_left_status + same_right_status)) -eq 1 ] || \
	fail "same-branch CAS did not produce exactly one conflict"
grep -qF "changed concurrently" \
	"$TEST_DIR/same.left.err" "$TEST_DIR/same.right.err" || \
	fail "same-branch CAS conflict was not reported"
run_fyai branch show cas-same
assert_status 0
grep -Eq "same (left|right)" "$TEST_DIR/stdout" || \
	fail "same-branch CAS lost both descriptions"

# A rename of HEAD keeps its intended HEAD after a disjoint CAS update.
run_fyai branch create cas-head
assert_status 0
run_fyai checkout cas-head
assert_status 0
CAS_RIGHT=(branch create cas-foreign)
run_cas_pair rename-head branch rename cas-head renamed-head
run_fyai root show
assert_status 0
assert_stdout_contains "renamed-head"
assert_stdout_not_contains "cas-head"
run_fyai checkout exp
assert_status 0

# The current branch cannot be deleted.
run_fyai branch delete exp
assert_status 1
assert_stderr_contains "current branch"

# --- reset moves the head and stays recoverable ---------------------------
run_fyai checkout main
assert_status 0

# HEAD^^ is the same as HEAD~2: back one full exchange.
run_fyai reset "HEAD^^"
assert_status 0
assert_state_contains "first prompt" dump state
assert_state_absent "second prompt" dump state

# Nothing was discarded: the ref log still holds the head that was there.
run_fyai list reflog
assert_status 0
assert_stdout_contains "reset"
run_fyai reset "main@{1}"
assert_status 0
assert_state_contains "second prompt" dump state

# The ref log records the operation rather than guessing it, and a rename
# keeps the name it was renamed from.
run_fyai branch rename ancient antique
assert_status 0
run_fyai -b antique list reflog
assert_status 0
assert_stdout_contains "rename"
assert_stdout_contains "ancient"

# A description is stored and listed.
run_fyai branch describe main "the trunk"
assert_status 0
run_fyai branch
assert_status 0
assert_stdout_contains "the trunk"

# --- a failed live switch leaves HEAD and the session on the old branch ----
# Inject a failure after the target is staged in memory but before checkout
# publishes. The rollback must restore both the session and durable HEAD.
run_fyai branch create bad-switch
assert_status 0

set +e
FYAI_TEST_BRANCH_SWITCH_FAIL=bad-switch \
"$FYAI_BIN" --color off --set display/markdown=false -i \
	>"$TEST_DIR/switch.stdout" 2>"$TEST_DIR/switch.stderr" <<'EOF'
/branch bad-switch
/exit
EOF
switch_status=$?
set -e
[ "$switch_status" -eq 0 ] || fail "REPL did not recover from failed switch"
grep -qF "injected switch failure" "$TEST_DIR/switch.stderr" || \
	fail "bad branch configuration did not reject the live switch"
run_fyai root show
assert_status 0
assert_stdout_contains "main"
assert_stdout_not_contains "bad-switch"

mock_stop 2
pass
