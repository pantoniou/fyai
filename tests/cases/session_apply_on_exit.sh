#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify what a session in a view leaves behind. A session that ends from an idle
# prompt applies its view to the project when no path conflicts. A conflict keeps
# the view, and so does view/apply_on_exit=false. A new session refuses to start
# while the view holds a change that the project does not have, and starts once
# the view is applied or removed.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view must lie outside the project.
fyai_test_scratch
export OPENAI_API_KEY=test-key

# The files of the case itself are in the project: they are not what it is about.
run_fyai config set view/ignore '["home","tmp","port","served","mock.log","requests.jsonl","pty.out","stdout","stderr","out","err","config.yaml","diff","watch","pty-script*","profile.out"]'
assert_status 0
run_fyai config set display/stream false
run_fyai config set tools true

SESSIONS=0
# session APPLY: one session on the branch $BRANCH, which a new call keeps. The
# first prompt edits the files; the steps on standard input run after it.
session() {
	local driver=0 steps first=true

	steps="$(cat)"
	[ "${KEEP:-}" = yes ] && first=false
	if $first; then
		SESSIONS=$((SESSIONS + 1))
		BRANCH="exit-$SESSIONS"
		"$FYAI_BIN" branch create "$BRANCH" >/dev/null 2>&1 || fail 'no branch'
		run_fyai -b "$BRANCH" config set sandbox_profile lockdown
		run_fyai -b "$BRANCH" config set api chat-completions
		run_fyai -b "$BRANCH" config set model mock-model
		run_fyai -b "$BRANCH" config set view/isolate_session true
	fi
	run_fyai -b "$BRANCH" config set view/apply_on_exit "$1"
	assert_status 0
	run_fyai -b "$BRANCH" config set api_url "$MOCK_URL/v1/chat/completions"
	{
	    if $first; then
		cat <<'PTY'
timeout: 60
steps:
  - type: edit the files
  - wait-frame: Session edit done.
    timeout: session
  - wait-screen: "\u276f"
    timeout: session
PTY
	    else
		printf 'timeout: 60\nsteps:\n'
	    fi
	    printf '%s\n' "$steps"
	} | pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b "$BRANCH" -i || driver=$?
	if [ "$driver" -ne 0 ]; then
		tail -c 3000 "$TEST_DIR/pty.out" >&2
		fail "the session did not finish"
	fi
}

reset_project() {
	printf baseline > file
	rm -f added
}

# An exit from an idle prompt applies the view.
reset_project
mock_start "$SCENARIOS_DIR/session_isolated.json"
KEEP=no session true <<'PTY'
  - write: "\x03"
PTY
[ "$(cat file)" = changed ] || fail 'the view was not applied when the session ended'
[ "$(cat added)" = new ] || fail 'the view did not add its file'
# Nothing is left to lose: a new session starts on a fresh view.
KEEP=yes session true <<'PTY'
  - type: /session status
  - wait-screen: "session view: session"
  - type: /exit
PTY
mock_stop 2

# A path that the project changed too is a conflict: nothing is applied, and a new
# session refuses to start.
reset_project
mock_start "$SCENARIOS_DIR/session_isolated.json"
rm -f "$TEST_DIR/watch"
( tail --pid=$$ -n +1 -F "$TEST_DIR/requests.jsonl" 2>/dev/null | head -n 2 >/dev/null
  printf other > "$TEST_DIR/file"
  printf done > "$TEST_DIR/watch" ) &
KEEP=no session true <<PTY
  - wait-file: $TEST_DIR/watch
    contains: done
  - type: /exit
PTY
wait $! 2>/dev/null || true
[ "$(cat file)" = other ] || fail 'a conflict was applied'
set +e
"$FYAI_BIN" -b "$BRANCH" -i </dev/null >"$TEST_DIR/refused.out" 2>&1
refused=$?
set -e
[ "$refused" -ne 0 ] || fail 'a session started over a view that conflicts'
grep -q 'changed differently' "$TEST_DIR/refused.out" || fail 'the refusal does not say that a path conflicts'
mock_stop 2

# With apply_on_exit off the changes stay in the view, and the next start refuses.
reset_project
mock_start "$SCENARIOS_DIR/session_isolated.json"
KEEP=no session false <<'PTY'
  - type: /exit
PTY
[ "$(cat file)" = baseline ] || fail 'the view was applied with apply_on_exit off'
run_fyai -b "$BRANCH" view diff session --stat
assert_stdout_contains file
# A start while the view holds changes is refused, with the way out.
set +e
"$FYAI_BIN" -b "$BRANCH" -i </dev/null >"$TEST_DIR/refused.out" 2>&1
refused=$?
set -e
[ "$refused" -ne 0 ] || fail 'a session started over a view that holds changes'
grep -q 'view apply session' "$TEST_DIR/refused.out" || fail 'the refusal does not name view apply'
grep -q 'view remove session' "$TEST_DIR/refused.out" || fail 'the refusal does not name view remove'
# Applying the view clears it.
run_fyai -b "$BRANCH" view apply session
assert_status 0
[ "$(cat file)" = changed ] || fail 'view apply did not write the file'
KEEP=yes session false <<'PTY'
  - type: /exit
PTY
mock_stop 2

for work in .fyai/views/*/work/work; do
	[ ! -d "$work" ] || chmod u+rwx "$work"
done

pass
