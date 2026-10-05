#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify /session push and /session pull in a session that runs in a view: push
# applies the changes of the session to the project, and a dry run writes nothing; pull
# replaces the view with the project as it is, refuses while the session holds what
# the project does not, and starts the session again on the fresh view.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view must lie outside the project.
export TMPDIR="$FYAI_TMPDIR_BASE"

"$FYAI_BIN" branch create lock >/dev/null 2>&1 || fail 'no branch'

# The files of the case itself are in the project: they are not what it is about.
IGNORE='view/ignore=["home","tmp","port","served","mock.log","requests.jsonl","pty.out","stdout","stderr","out","err","config.yaml","diff"]'

reset_project() {
	printf baseline > file
	rm -f added
}

# One session on the branch `lock`: the first prompt edits the files, then the steps
# of $1 run on what the screen shows.
session() {
	local after="$1" driver=0
	FYAI_PTY_INPUT="edit the files" FYAI_PTY_NEEDLE="Session edit done." FYAI_PTY_TIMEOUT=60 \
	FYAI_PTY_AFTER="$after" \
	"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	    "$FYAI_BIN" -b lock -k test-key --set view/isolate_session=true --set "$IGNORE" \
	    --set display/stream=false --set tools=true --set api=chat-completions \
	    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i || driver=$?
	if [ "$driver" -ne 0 ]; then
		tail -c 3000 "$TEST_DIR/pty.out" >&2
		fail "the session did not finish: $after"
	fi
}

# A dry run reports and writes nothing.
reset_project
mock_start session_isolated.json
session "send:/session push --dry-run|wait-screen:would|send:/exit"
[ "$(cat file)" = baseline ] || fail 'a dry run changed the project'
[ ! -e added ] || fail 'a dry run added a file'
mock_stop 2

# push writes the changes of the session into the project, while the session runs.
reset_project
mock_start session_isolated.json
session "send:/session push|wait-screen:applied|send:/exit"
[ "$(cat file)" = changed ] || fail 'push did not write the file'
[ "$(cat added)" = new ] || fail 'push did not write the added file'
mock_stop 2

# pull refuses while the session holds what the project does not.
reset_project
mock_start session_isolated.json
session "send:/session pull|wait-screen:would be dropped|send:/exit"
[ "$(cat file)" = baseline ] || fail 'a refused pull changed the project'
mock_stop 2

# After a push nothing is left to drop: pull replaces the view and starts the session
# again, and the new view holds no change against the project that now has them.
reset_project
mock_start session_isolated.json
session "send:/session push|wait-screen:applied|send:/session pull|wait-screen:starts again|send:/exit"
[ "$(cat file)" = changed ] || fail 'push did not write the file'
run_fyai -b lock view diff session --stat
assert_status 0
assert_stdout_not_contains 'modified'
assert_stdout_not_contains 'added'
mock_stop 2

# --discard drops what the session holds, and the project stays as it was.
reset_project
mock_start session_isolated.json
session "send:/session pull --discard|wait-screen:starts again|send:/exit"
[ "$(cat file)" = baseline ] || fail 'pull --discard changed the project'
run_fyai -b lock view diff session --stat
assert_status 0
assert_stdout_not_contains 'modified'
mock_stop 2

pass
