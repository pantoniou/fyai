#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify /session profile with a profile that carries a view setting. A session in
# a view that changes to a profile without one applies its view to the project
# first and starts again outside it; a conflict refuses the switch and changes
# nothing. A session outside a view starts again in one for a profile that asks
# for it.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view must lie outside the project.
fyai_test_scratch

# The files of the case itself are in the project: they are not what it is about.
run_fyai config set view/ignore '["home","tmp","port","served","mock.log","requests.jsonl","pty.out","stdout","stderr","out","err","config.yaml","diff","watch"]'
assert_status 0
run_fyai config set display/stream false
run_fyai config set tools true

# The credential comes from the environment: a session that starts again has no
# command line to carry it.
export OPENAI_API_KEY=test-key

SESSIONS=0
# session PROFILE VIEW: one session on a branch of its own, with the profile and
# the view stored. The steps on standard input run on what the screen shows.
session() {
	local driver=0 steps

	steps="$(cat)"
	SESSIONS=$((SESSIONS + 1))
	BRANCH="prof-$SESSIONS"
	"$FYAI_BIN" branch create "$BRANCH" >/dev/null 2>&1 || fail 'no branch'
	run_fyai -b "$BRANCH" config set sandbox_profile "$1"
	assert_status 0
	run_fyai -b "$BRANCH" config set view/isolate_session "$2"
	assert_status 0
	# The configuration is stored: a session that starts again reads it from the arena.
	run_fyai -b "$BRANCH" config set api chat-completions
	assert_status 0
	run_fyai -b "$BRANCH" config set model mock-model
	assert_status 0
	run_fyai -b "$BRANCH" config set api_url "$MOCK_URL/v1/chat/completions"
	assert_status 0
	{
	    cat <<'PTY'
timeout: 60
steps:
  - type: edit the files
  - wait-frame: Session edit done.
    timeout: session
  - wait-screen: "\u276f"
    timeout: session
PTY
	    printf '%s\n' "$steps"
	} | pty_run "$TEST_DIR/pty.out" \
	    "$FYAI_BIN" -b "$BRANCH" -i || driver=$?
	if [ "$driver" -ne 0 ]; then
		tail -c 3000 "$TEST_DIR/pty.out" >&2
		fail "the session did not finish"
	fi
}

reset_project() {
	printf baseline > file
	rm -f added
}

# A profile without a view: the view applies cleanly, and the session starts again outside it.
reset_project
mock_start "$SCENARIOS_DIR/session_isolated.json"
session lockdown true <<'PTY'
  - type: "/session profile dev"
  # The session ends and a new one starts, outside the view.
  - wait-count: "\e[>1u"
    more: 1
  - type: "/session status"
  - wait-screen: "session view: none"
  - type: "/exit"
PTY
[ "$(cat file)" = changed ] || fail 'the view was not applied to the project'
[ "$(cat added)" = new ] || fail 'the view did not add its file'
run_fyai -b "$BRANCH" config get sandbox_profile
assert_stdout_contains dev
run_fyai -b "$BRANCH" config get view/isolate_session
assert_stdout_contains false
mock_stop 2

# A path that the project and the view changed differently refuses the switch.
reset_project
mock_start "$SCENARIOS_DIR/session_isolated.json"
rm -f "$TEST_DIR/watch"
# The second request carries the result of the edit: the view holds it, and the
# project changes the same file after it.
( tail --pid=$$ -n +1 -F "$TEST_DIR/requests.jsonl" 2>/dev/null | head -n 2 >/dev/null
  printf other > "$TEST_DIR/file"
  printf done > "$TEST_DIR/watch" ) &
WATCH=$!
session lockdown true <<PTY
  - wait-file: $TEST_DIR/watch
    contains: done
  - type: "/session profile dev"
  - wait-screen: "cannot apply cleanly"
  - type: "/session status"
  - wait-screen: "session view: session"
  - type: "/exit"
PTY
wait "$WATCH" 2>/dev/null || true
[ "$(cat file)" = other ] || fail 'a refused switch changed the project'
run_fyai -b "$BRANCH" config get sandbox_profile
assert_stdout_contains lockdown
run_fyai -b "$BRANCH" config get view/isolate_session
assert_stdout_contains true
mock_stop 2

# A session outside a view starts again in one for a profile that asks for it.
reset_project
mock_start "$SCENARIOS_DIR/session_isolated.json"
session dev false <<'PTY'
  - type: "/session profile lockdown"
  - wait-count: "\e[>1u"
    more: 1
  - type: "/session status"
  - wait-screen: "session view: session"
  - type: "/exit"
PTY
mock_stop 2

for work in .fyai/views/*/work/work; do
	[ ! -d "$work" ] || chmod u+rwx "$work"
done

pass
