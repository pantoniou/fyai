# SPDX-License-Identifier: MIT
# An oracle case runs one PTY script two times: in a local session, which is
# the oracle, and in a session of `fyai serve` through `--remote .`. The two
# runs start from the same sandbox, at the same paths, with the same replies
# of the mock. Each `checkpoint: NAME` step of the script records the screen,
# and the case fails when a screen of the remote run differs from the screen
# of the local run.
set -eu
. "$(dirname "${BASH_SOURCE[0]}")/remote-harness.sh"

# oracle_setup SCENARIO: a sandbox with the mock provider; nothing serves it.
oracle_setup() {
	fyai_test_setup
	ORACLE_SCENARIO="$1"
	mock_start "$ORACLE_SCENARIO"
	export MOCK_KEY=test-key
	"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
		--set "api_url=$MOCK_URL/v1/chat/completions" \
		--set model=mock-model \
		--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
	ORACLE_DIR="$TEST_DIR/oracle"
	mkdir -p "$ORACLE_DIR"
}

# oracle_script DIR [EXPECT]: the script on standard input, with each
# checkpoint name made a path in DIR. With EXPECT, the directory of the
# checkpoints of the other run, a checkpoint waits for the screen of that run
# before it records its own.
oracle_script() {
	if [ -n "${2:-}" ]; then
		sed -E "s|^([[:space:]]*)-([[:space:]]*)checkpoint:[[:space:]]*(.*)$|\\1-\\2checkpoint: $1/\\3\\n\\1 \\2expect: $2/\\3|"
	else
		sed -E "s|^([[:space:]]*-[[:space:]]*checkpoint:[[:space:]]*)(.*)$|\\1$1/\\2|"
	fi
}

# oracle_compare ARGS...: run the script on standard input as `fyai ARGS`, then
# as `fyai --remote . ARGS`, and compare the checkpoints. The remote client has
# no credential: the server holds it.
oracle_compare() {
	local script="$ORACLE_DIR/script.yaml" name names=0 diffs=0

	cat >"$script"
	mkdir -p "$ORACLE_DIR/local" "$ORACLE_DIR/remote"
	"$PYTHON" "$TESTS_DIR/oracle_sandbox.py" keep "$TEST_DIR" \
		"$ORACLE_DIR/sandbox"

	oracle_script "$ORACLE_DIR/local" <"$script" |
		pty_run "$ORACLE_DIR/local.out" "$FYAI_BIN" "$@" ||
		fail "the local run failed"

	# The second run starts where the first one started.
	mock_stop_quiet
	"$PYTHON" "$TESTS_DIR/oracle_sandbox.py" restore "$TEST_DIR" \
		"$ORACLE_DIR/sandbox"
	mock_start "$ORACLE_SCENARIO"
	"$FYAI_BIN" "$@" --set "api_url=$MOCK_URL/v1/chat/completions" >/dev/null 2>&1 ||
		fail "cannot point the second run at its mock"
	remote_serve_start
	oracle_script "$ORACLE_DIR/remote" "$ORACLE_DIR/local" <"$script" |
		pty_run "$ORACLE_DIR/remote.out" "$(command -v env)" -u MOCK_KEY \
			"$FYAI_BIN" --remote . "$@" ||
		fail "the remote run failed"
	remote_test_stop

	for name in "$ORACLE_DIR"/local/*; do
		[ -e "$name" ] || continue
		names=$((names + 1))
		name="${name##*/}"
		[ -f "$ORACLE_DIR/remote/$name" ] ||
			fail "the remote run has no checkpoint '$name'"
		if ! diff -u --label "local/$name" --label "remote/$name" \
			"$ORACLE_DIR/local/$name" "$ORACLE_DIR/remote/$name" \
			>"$ORACLE_DIR/$name.diff"; then
			diffs=$((diffs + 1))
			printf 'checkpoint %s differs:\n' "$name" >&2
			cat "$ORACLE_DIR/$name.diff" >&2
		fi
	done
	[ "$names" -gt 0 ] || fail "the script has no checkpoint"
	[ "$diffs" -eq 0 ] || fail "$diffs of $names checkpoints differ from the local run"
}
