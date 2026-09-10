#!/bin/bash
# SPDX-License-Identifier: MIT
# An interactive session that names no branch starts a session of its own, so
# the conversation the last session left is kept and is resumed deliberately.
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
run_fyai --set model=foo
assert_status 0
# In the arena, not on the command line: a --set is a durable change, and it
# would publish the session branch this case is checking is absent.
run_fyai --set display/stream=false
assert_status 0

export MOCKPROV_API_KEY=mock-secret

run_session() {
	set +e
	"$FYAI_BIN" --color off "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
	FYAI_STATUS=$?
	set -e
}

# A wide listing, so a session name is not truncated to fit the table.
list_sessions() {
	COLUMNS=200 "$FYAI_BIN" --color off branch --all 2>/dev/null | \
		grep -o "session/[^ ]*" || true
}

sessions() {
	list_sessions | grep -c . || true
}

# --- an empty interactive run stores nothing -----------------------------
echo "/exit" | run_session -m foo -i
assert_status 0
[ "$(sessions)" -eq 0 ] || fail "an empty session was stored"

# --- two runs are two separate sessions ----------------------------------
echo "first session" | run_session -m foo -i
assert_status 0
echo "second session" | run_session -m foo -i
assert_status 0
[ "$(sessions)" -eq 2 ] || fail "the two runs did not make two sessions"

# Each session holds its own conversation and neither is on main.
first=$(list_sessions | sed -n 1p)
second=$(list_sessions | sed -n 2p)
[ -n "$first" ] && [ -n "$second" ] || fail "the sessions were not named"
[ "$first" != "$second" ] || fail "the two sessions share a name"
assert_state_absent "first session" dump state
assert_state_absent "second session" dump state

# A session takes the configuration of the branch HEAD names, not its turns.
run_fyai -b "$first" config get model
assert_status 0
assert_stdout_contains "foo"

# Starting a session does not move HEAD.
run_fyai root show
assert_status 0
assert_stdout_contains "main"

# --- the exceptions keep their behaviour ---------------------------------
# A batch prompt continues the branch it is given, as it always did.
run_session -m foo "batch prompt"
assert_status 0
assert_state_contains "batch prompt" dump state
[ "$(sessions)" -eq 2 ] || fail "a batch prompt started a session"

# A named branch is where the session works.
echo "named branch" | run_session -m foo -b main -i
assert_status 0
assert_state_contains "named branch" dump state
[ "$(sessions)" -eq 2 ] || fail "an explicit branch started a session"

# $FYAI_BRANCH names one the same way.
run_fyai branch create env-branch
assert_status 0
echo "env branch" | FYAI_BRANCH=env-branch run_session -m foo -i
assert_status 0
assert_state_contains "env branch" -b env-branch dump state
[ "$(sessions)" -eq 2 ] || fail "\$FYAI_BRANCH started a session"

# --new still clears the branch it is given rather than starting a session.
echo "/exit" | run_session -m foo --new -b main -i
assert_status 0
assert_state_absent "batch prompt" dump state
[ "$(sessions)" -eq 2 ] || fail "--new started a session"

# --- a stored session is resumable ---------------------------------------
echo "back again" | run_session -m foo resume "$first"
assert_status 0
assert_state_contains "first session" -b "$first" dump state
assert_state_contains "back again" -b "$first" dump state

mock_stop 6
pass
