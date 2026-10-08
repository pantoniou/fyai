#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that view enter --lockdown confines the command as a tool call of a
# session under /session lockdown: the lockdown profile denies ~/.ssh and
# network egress, and the command can still change the root of the project.
# Without the option, the view alone confines the command.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"
FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
# The scratch directory of a view lies outside the project and does not cover HOME.
mkdir -p tmp project/src "$HOME/.ssh"
export TMPDIR="$TEST_DIR/tmp"

printf 'secret\n' > "$HOME/.ssh/id"
printf 'main\n' > project/src/main.c
# A listener on localhost, for the egress check.
mock_start agent_sandbox.json

run_fyai view create demo project
assert_status 0

# The environment of the command is sanitized: the port goes in the text.
probe='cat "$HOME/.ssh/id" >/dev/null 2>&1 && echo ssh-readable || echo ssh-denied
bash -c "exec 3<>/dev/tcp/127.0.0.1/'"$MOCK_PORT"'" 2>/dev/null && echo net-open || echo net-denied
touch new && mv new renamed && rm renamed && mkdir d && rmdir d && echo root-writable
touch src/new && rm src/main.c src/new && echo below-writable; true'

run_fyai view enter demo sh -c "$probe"
assert_status 0
assert_stdout_contains 'ssh-readable'
assert_stdout_contains 'net-open'
assert_stdout_contains 'root-writable'
assert_stdout_contains 'below-writable'

run_fyai view enter --lockdown demo sh -c "$probe"
assert_status 0
assert_stdout_contains 'ssh-denied'
assert_stdout_contains 'root-writable'
# The lockdown profile restricts egress where the kernel can.
if grep -q 'cannot restrict network egress' "$TEST_DIR/stderr"; then
	assert_stdout_contains 'net-open'
else
	assert_stdout_contains 'net-denied'
fi

# The changes stay in the view.
[ -f project/src/main.c ] || fail 'the view changed the project'

mock_stop
pass
