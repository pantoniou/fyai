#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify how a sandboxed tool hides the project arena: in a user namespace of the
# tool, in the namespaces of a bubblewrap holder, or not at all, where Landlock
# alone denies the arena and a tool cannot create entries in the project root. A
# host that cannot hide the arena says so at the start, and names the fix.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

# The sandbox grants the system scratch trees whole: keep the project out of them.
FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unset FYAI_TEST_NO_HOST_WARNINGS
mkdir -p "$HOME/.ssh"
printf 'classified\n' >"$HOME/.ssh/id_test"

# cover_shell MODE COMMAND [WRAPPER...]: run a command in the shell tool with the
# profile dev and the cover MODE, in a configuration file that is not stored.
cover_shell() {
	local mode="$1" command="$2"

	shift 2
	printf 'sandbox_profile: dev\nsandbox_cover: %s\n' "$mode" >"$TEST_DIR/cover.yaml"
	set +e
	"$@" "$FYAI_BIN" -k test-key --color off --config "$TEST_DIR/cover.yaml" \
		tool shell '{"command": "'"$command"'"}' \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
}

# A host that cannot make a user namespace: a namespace that allows no further one.
restricted() {
	unshare -Ur sh -c 'echo 0 > /proc/sys/user/max_user_namespaces && exec "$@"' sh "$@"
}
HAVE_RESTRICTED=no
if restricted true 2>/dev/null && ! restricted unshare -U true 2>/dev/null; then
	HAVE_RESTRICTED=yes
fi

CHECK='touch made-$$ && echo root-ok; test -z $(ls -A .fyai 2>/dev/null) && echo arena-hidden; echo x > .fyai/probe || echo arena-readonly; cat ~/.ssh/id_test || echo secret-hidden'

# Each cover that the host has hides the arena and the denied paths, and leaves the
# project root writable.
for mode in userns bwrap auto; do
	# The host may not have this cover: ask fyai, which is what counts.
	cover_shell "$mode" 'touch cover-probe && rm -f cover-probe && echo cover-works'
	grep -q cover-works "$TEST_DIR/stdout" || continue
	cover_shell "$mode" "$CHECK"
	assert_status 0
	assert_stdout_contains root-ok
	assert_stdout_contains arena-hidden
	assert_stdout_contains arena-readonly
	assert_stdout_not_contains classified
	[ ! -e .fyai/probe ] || fail "$mode: a tool wrote into the arena"
	# The tool is in a PID namespace of its own.
	cover_shell "$mode" 'echo pid=$$'
	pid="$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$TEST_DIR/stdout" | head -n 1)"
	[ -n "$pid" ] && [ "$pid" -lt 20 ] || fail "$mode: process id $pid is not one of a PID namespace"
	# A report of an unusable cover is not given where a cover works.
	! grep -q 'does not let fyai hide' "$TEST_DIR/stderr" || fail "$mode: a report that the cover is unusable"
done

# A host without a cover: Landlock denies the arena, and the root takes no new entry.
if [ "$HAVE_RESTRICTED" = yes ]; then
	cover_shell auto "$CHECK" restricted
	assert_stdout_not_contains root-ok
	assert_stdout_contains arena-readonly
	assert_stdout_not_contains classified
	# It says so once, with both fixes.
	grep -q 'does not let fyai hide the project arena' "$TEST_DIR/stderr" || fail 'no report that the arena cannot be hidden'
	grep -q 'fyai sandbox apparmor' "$TEST_DIR/stderr" || fail 'the report does not name the AppArmor profile'
	grep -q 'bubblewrap' "$TEST_DIR/stderr" || fail 'the report does not name bubblewrap'
	[ "$(grep -c 'does not let fyai hide the project arena' "$TEST_DIR/stderr")" -eq 1 ] || fail 'the report repeats'
	# sandbox_cover none is the choice to go without, and says no more.
	cover_shell none "$CHECK" restricted
	! grep -q 'does not let fyai hide' "$TEST_DIR/stderr" || fail 'none still reports'
fi

# The profile names the binary that runs, and parses where AppArmor is present.
"$FYAI_BIN" sandbox apparmor >"$TEST_DIR/apparmor.out" 2>"$TEST_DIR/stderr" || fail 'fyai sandbox apparmor'
grep -q "^profile fyai $(readlink -f "$FYAI_BIN") flags=(unconfined)" "$TEST_DIR/apparmor.out" ||
	fail 'the profile does not name this binary'
grep -q '^  userns,' "$TEST_DIR/apparmor.out" || fail 'the profile does not grant userns'
if command -v apparmor_parser >/dev/null 2>&1; then
	apparmor_parser -QTK "$TEST_DIR/apparmor.out" 2>"$TEST_DIR/stderr" || fail 'the profile does not parse'
fi
pass
