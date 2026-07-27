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

mock_stop 2
pass
