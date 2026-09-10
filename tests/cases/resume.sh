#!/bin/bash
# SPDX-License-Identifier: MIT
# The resume verb continues a stored session: by name, or the most recently
# updated one. It selects the session for one invocation, so HEAD does not
# move, and it reports when there is nothing to resume.
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

# Run an interactive line on a branch, as a user continuing a session does.
run_session() {
	set +e
	"$FYAI_BIN" --color off --set display/markdown=false \
		--set display/stream=false "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
	FYAI_STATUS=$?
	set -e
}

# --- argument checking ---------------------------------------------------
run_fyai resume one two
assert_status 1
assert_stderr_contains "unexpected argument"

run_fyai resume --last somebranch
assert_status 1
assert_stderr_contains "does not take a branch"

run_fyai resume --bogus
assert_status 1
assert_stderr_contains "unknown option"

# --- two sessions in this directory --------------------------------------
run_fyai --set model=foo
assert_status 0
run_fyai branch create older
assert_status 0
run_fyai branch create newer
assert_status 0

# Each session keeps settings of its own, and HEAD keeps different ones.
run_fyai -b older --set temperature=0.11
assert_status 0
run_fyai -b newer --set temperature=0.22
assert_status 0
run_fyai --set temperature=0.33
assert_status 0

echo "on older" | run_session -m foo -b older -i
assert_status 0
echo "on newer" | run_session -m foo -b newer -i
assert_status 0

# --- resume by name continues that session's conversation ----------------
echo "again on older" | run_session -m foo resume older
assert_status 0
assert_state_contains "on older" -b older dump state
assert_state_contains "again on older" -b older dump state
# The turn went to the named branch and nowhere else.
assert_state_absent "again on older" -b newer dump state

# The request carries the resumed branch's own setting. The configuration is
# read for the branch the invocation selects, not for the one HEAD names.
assert_request 2 "abs(r['body'].get('temperature', 0) - 0.11) < 1e-6"

# A named resume selects for one invocation only: HEAD stays where it was.
run_fyai root show
assert_status 0
assert_stdout_contains "main"

# --- --last resumes the most recently updated session --------------------
# `older` was resumed last, so it is now the newest.
echo "latest turn" | run_session -m foo resume --last
assert_status 0
assert_state_contains "latest turn" -b older dump state
assert_state_absent "latest turn" -b newer dump state

run_fyai root show
assert_status 0
assert_stdout_contains "main"

# --- a session of another directory is not offered here ------------------
# The arena is found by walking up, so a subdirectory shares this one while
# starting in a directory of its own.
mkdir -p elsewhere
set +e
( cd elsewhere && "$FYAI_BIN" -k test-key --color off resume --last \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null )
status=$?
set -e
[ "$status" -ne 0 ] || fail "resume --last accepted a foreign directory"
grep -qF "no resumable session" "$TEST_DIR/stderr" || \
	fail "resume --last did not report the empty selection"

# --all reaches the sessions of every directory.
set +e
( cd elsewhere && echo "from elsewhere" | "$FYAI_BIN" -k test-key \
	--color off --set display/markdown=false --set display/stream=false \
	-m foo resume --last --all \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" )
status=$?
set -e
[ "$status" -eq 0 ] || fail "resume --last --all did not resume"
assert_state_contains "from elsewhere" -b older dump state

mock_stop 5
pass
