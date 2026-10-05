#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify view/tool_diff: a group of tool calls that ran a program shows what it
# changed in the project as a diff under the calls, the diff is stored and replays
# in the history, a path that the ignore rules name is left out, and `undo` takes the
# project back.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# The files of the case itself are in the project: they are not what it is about.
IGNORE='view/ignore=["port","served","mock.log","requests.jsonl","stdout","stderr","out","err","tmp","config.yaml"]'

reset_project() {
	printf 'alpha line\nkeep\n' > existing.txt
	printf 'ignored.log\n' > .gitignore
	rm -f made.txt ignored.log first.txt second.txt
}

run_with() {
	run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
		 --set "$IGNORE" --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "$@"
}

# Off by default: the calls run, and nothing is compared.
reset_project
mock_start tool_diff.json
run_with "edit the files"
assert_status 0
assert_stdout_contains 'All done.'
assert_stdout_not_contains 'diff --git'
[ "$(cat made.txt)" = hello ] || fail 'the call did not run'

# On: the change of the call is shown, and an ignored path is not.
reset_project
mock_start tool_diff.json
run_with --set view/tool_diff=true "edit the files"
assert_status 0
assert_stdout_contains 'diff --git a/made.txt b/made.txt'
assert_stdout_contains '+hello'
assert_stdout_contains '-alpha line'
assert_stdout_contains '+beta line'
assert_stdout_not_contains 'ignored.log'
assert_stdout_not_contains 'requests.jsonl'
assert_stdout_contains 'All done.'

# The diff is part of the record, and the history draws it again.
run_fyai history
assert_status 0
assert_stdout_contains 'diff --git a/made.txt b/made.txt'
assert_stdout_contains '+beta line'

# A patch does not display its own diff while the group diff shows it: the call names
# the file, and the change is shown once, under the calls. Without the diff it shows
# its own. The list and project_view calls have a title and not their arguments.
reset_project
mock_start tool_diff_patch.json
run_with --new --set view/tool_diff=true "patch the file"
assert_status 0
assert_stdout_contains 'diff --git a/existing.txt b/existing.txt'
assert_stdout_contains 'Patched.'
[ "$(head -n 1 existing.txt)" = 'beta line' ] || fail 'the patch did not run'
assert_stdout_not_contains '{"kind"'
assert_stdout_not_contains '{"action"'
# The record holds the change one time, and the history draws it one time.
run_fyai history
assert_status 0
[ "$(grep -c -- '+beta line' "$TEST_DIR/stdout")" = 1 ] || fail 'the patch is in the record twice'
assert_stdout_not_contains '{"kind"'
assert_stdout_not_contains '{"action"'
reset_project
mock_start tool_diff_patch.json
run_with --new "patch the file"
assert_status 0
assert_stdout_contains 'diff --git a/existing.txt b/existing.txt'
assert_stdout_not_contains '{"kind"'
run_fyai history
assert_status 0
[ "$(grep -c -- '+beta line' "$TEST_DIR/stdout")" = 1 ] || fail 'the patch is in the record twice'

# undo takes the project back: the file is gone and the line is as it was. A second
# undo has nothing to take back.
reset_project
mock_start tool_diff.json
set +e
printf 'edit the files\n/undo\n/undo\n/exit\n' | "$FYAI_BIN" -k test-key --color off \
	--set api=chat-completions --set display/stream=false --set tools=true \
	--set view/tool_diff=true --set "$IGNORE" --set display/markdown=false \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model -i >"$TEST_DIR/out" 2>"$TEST_DIR/err"
set -e
grep -qF 'diff --git a/made.txt b/made.txt' "$TEST_DIR/out" || fail 'the session showed no diff'
grep -qE '[1-9][0-9]* paths restored, 0 left as changed since' "$TEST_DIR/out" || fail 'undo reported nothing'
[ ! -e made.txt ] || fail 'undo left the new file'
[ "$(head -n 1 existing.txt)" = 'alpha line' ] || fail 'undo did not restore the line'
[ -e ignored.log ] || fail 'undo touched an ignored path'
grep -qF 'no tool call of this session changed the project' "$TEST_DIR/err" "$TEST_DIR/out" || fail 'a second undo had a change to take back'

# view/undo_depth bounds the groups that undo can take back: with a depth of one, the
# newest group is taken back and the older one stays.
reset_project
mock_start tool_diff_two.json
set +e
printf 'make the first file\nmake the second file\n/undo\n/undo\n/exit\n' | "$FYAI_BIN" -k test-key --color off \
	--set api=chat-completions --set display/stream=false --set tools=true \
	--set view/tool_diff=true --set view/undo_depth=1 --set "$IGNORE" --set display/markdown=false \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model -i >"$TEST_DIR/out" 2>"$TEST_DIR/err"
set -e
[ ! -e second.txt ] || fail 'undo left the newest file'
[ -e first.txt ] || fail 'undo went deeper than view/undo_depth'
grep -qF 'no tool call of this session changed the project' "$TEST_DIR/err" "$TEST_DIR/out" || fail 'the group beyond the depth was kept'
mock_stop 4

# A session that runs in a view takes the diffs too, and the project is not changed.
# Its states are kept in a directory of the arena for the run, which is gone at the end.
# The note that closes the session does not list a path that the project ignores.
# A session in a view cannot use credential isolation, so the transport run skips it.
if [ -z "${FYAI_TEST_TRANSPORT:-}" ] && unshare -Urnm true 2>/dev/null; then
	reset_project
	mock_start tool_diff.json
	TMPDIR="$(dirname "$FYAI_BIN")" run_with --set view/tool_diff=true --set view/isolate_session=true \
		"edit the files"
	assert_status 0
	assert_stdout_contains 'diff --git a/made.txt b/made.txt'
	assert_stdout_contains '+beta line'
	assert_stdout_not_contains 'ignored.log'
	assert_stderr_contains 'The session ran in the view'
	assert_stderr_contains 'added made.txt'
	! grep -qE '^  (added|modified|deleted) ignored.log' "$TEST_DIR/stderr" || fail 'the note lists an ignored path'
	[ ! -e made.txt ] || fail 'the session changed the project'
	[ ! -e .fyai/arena/tooldiff ] || fail 'the states of the run were not removed'
	mock_stop 2
fi

pass
