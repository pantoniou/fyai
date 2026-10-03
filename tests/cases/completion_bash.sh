#!/bin/bash
# SPDX-License-Identifier: MIT
# The bash completion script asks fyai __complete and fills COMPREPLY. It
# works outside a project too, and does not make one.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai completion bash
assert_status 0
assert_stdout_contains "complete -F _fyai_complete fyai"
cp "$TEST_DIR/stdout" "$TEST_DIR/fyai.bash"

# complete_line WORD...: the candidates for the last word, one on each line,
# then the nospace mark when the script set it.
complete_line() {
	FYAI_BIN="$FYAI_BIN" bash -c '
		compopt() { [ "$2" = nospace ] && nospace=1; return 0; }
		. "$1"
		shift
		COMP_WORDS=("$FYAI_BIN" "$@")
		COMP_CWORD=$#
		nospace=
		_fyai_complete
		printf "%s\n" "${COMPREPLY[@]}"
		[ -z "$nospace" ] || echo "<nospace>"
	' complete "$TEST_DIR/fyai.bash" "$@"
}

complete_line branch "" >"$TEST_DIR/words"
grep -qx new "$TEST_DIR/words" || fail "branch: no subcommand new"
grep -qx main "$TEST_DIR/words" || fail "branch: no branch main"

complete_line branch delete --f >"$TEST_DIR/words"
grep -qx -- --force "$TEST_DIR/words" || fail "branch delete: no --force"

complete_line --set displ >"$TEST_DIR/words"
grep -qx display/ "$TEST_DIR/words" || fail "--set: no display/"
grep -qx "<nospace>" "$TEST_DIR/words" || fail "--set: a space follows a path"

complete_line config set reasoning/effort med >"$TEST_DIR/words"
grep -qx medium "$TEST_DIR/words" || fail "config set: no value medium"

if [ "$(uname -s)" = Linux ]; then
	mkdir view-project
	run_fyai view create test-view "$TEST_DIR/view-project"
	assert_status 0
	for verb in show update remove sync mount unmount enter; do
		complete_line view "$verb" te >"$TEST_DIR/words"
		grep -qx test-view "$TEST_DIR/words" || fail "view $verb: no view name"
	done
	run_fyai view remove test-view
	assert_status 0
	complete_line view enter te >"$TEST_DIR/words"
	if grep -qx test-view "$TEST_DIR/words"; then fail "removed view still completes"; fi
fi

# Outside a project, completion gives the commands and makes no project.
fyai_test_setup_bare
run_fyai completion bash
assert_status 0
cp "$TEST_DIR/stdout" "$TEST_DIR/fyai.bash"
complete_line bra >"$TEST_DIR/words"
grep -qx branch "$TEST_DIR/words" || fail "no project: no command branch"
complete_line checkout "" >"$TEST_DIR/words"
grep -qx HEAD "$TEST_DIR/words" || fail "no project: no reference HEAD"
[ ! -e "$TEST_DIR/.fyai" ] || fail "completion made a project"

pass
