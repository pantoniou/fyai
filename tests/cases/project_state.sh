#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that view/track_project records the project state with each ref-log
# entry that moves the head, and shares a state that did not change.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
# The project is a directory of its own: the harness writes its captures in the
# directory of the case, and they would change the state.
mkdir project
cd project
printf 'display:\n  markdown: false\n  renderer: stack\n  screen: inline\n  work_controls: none\n  work_zoom_rows: full\n' > config.yaml
"$FYAI_BIN" init >/dev/null 2>&1 || fail "fyai init"
rm -f config.yaml
printf one > file
mock_start project_state.json

ask() {
	run_fyai --set view/track_project=true --set api=chat-completions \
		 --set display/stream=false --set tools=false \
		 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "$1"
	assert_status 0
}
ask first
printf two > file
ask second
ask third

run_fyai list reflog --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY'
import json, sys
rows = json.load(open(sys.argv[1]))
turns = [r for r in rows if r['kind'] == 'turn']
states = [r['project'] for r in turns]
# Newest first, two entries for each prompt: the third and the second share
# the state after the edit, and the first has the state before it.
assert len(turns) >= 6, rows
assert all(len(s) == 12 for s in states[:6]), states
assert len(set(states[:4])) == 1, states
assert states[3] != states[4], states
assert states[4] == states[5], states
PY

# A reference names the files of its point: the edit is between the first
# answer and the last, and a view diff takes the references as it takes views.
run_fyai view diff HEAD~4 HEAD --stat
assert_status 0
assert_stdout_contains 'modified file'
run_fyai view diff HEAD~2 HEAD --stat
assert_status 0
run_fyai view diff 'main@{4}' 'main@{0}' --stat
assert_status 0
assert_stdout_contains 'modified file'

mock_stop 3
pass
