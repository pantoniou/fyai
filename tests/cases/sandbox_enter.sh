#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify `sandbox enter`: a command runs confined by a built-in profile, with no
# project and no stored state, and its exit status is the status of the verb.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup_bare
# The bare setup runs no `fyai init`: there is no project and no arena.
[ ! -e .fyai ]

mkdir -p "$HOME/.ssh"
printf 'classified\n' >"$HOME/.ssh/id_test"

# The built-in profiles are listed with no project.
run_fyai sandbox profiles
assert_status 0
assert_stdout_contains lockdown
assert_stdout_contains dev

# One profile is shown as the policy it resolves to, also with no project.
run_fyai sandbox show lockdown
assert_status 0
assert_stdout_contains 'enabled: true'
assert_stdout_contains 'network:'
run_fyai sandbox show nosuch
assert_status 1
# `none` confines nothing, so there is no policy to show or to enter.
run_fyai sandbox show none
assert_status 1
run_fyai sandbox enter --profile none -- sh -c 'echo started'
assert_status 1
assert_stdout_not_contains started

# The default profile runs the command, and denies the secrets of the user.
run_fyai sandbox enter -- sh -c 'cat ~/.ssh/id_test || echo deny-ok'
assert_status 0
assert_stdout_contains deny-ok
assert_stdout_not_contains classified

# The exit status is the status of the command.
run_fyai sandbox enter -- sh -c 'exit 7'
assert_status 7

# A command that does not exist is reported as a shell does.
run_fyai sandbox enter -- /nonexistent/program
assert_status 127

# A profile that does not exist stops the run before the command starts.
run_fyai sandbox enter --profile nosuch -- sh -c 'echo started'
assert_status 1
assert_stdout_not_contains started

# The lockdown profile takes an option before the command. UDP is denied where
# the kernel can deny it, with Landlock ABI 10 or with the seccomp filter.
run_fyai sandbox enter --profile lockdown -- sh -c \
	'bash -c "exec 3<>/dev/udp/127.0.0.1/9" 2>/dev/null && echo udp-open || echo udp-denied'
assert_status 0
if grep -q 'UDP egress remains open' "$TEST_DIR/stderr"; then
	assert_stdout_contains udp-open
else
	assert_stdout_contains udp-denied
fi

# With a project, the stored configuration decides. The harness stores
# `sandbox_profile: none`, which confines nothing, so the built-in default
# applies until the configuration names a profile.
UDP='bash -c "exec 3<>/dev/udp/127.0.0.1/9" 2>/dev/null && echo udp-open || echo udp-denied'
fyai_test_setup
run_fyai sandbox enter -- sh -c "$UDP"
assert_status 0
assert_stdout_contains udp-open

run_fyai config set sandbox_profile lockdown
assert_status 0
run_fyai sandbox enter -- sh -c "$UDP"
assert_status 0
if grep -q 'UDP egress remains open' "$TEST_DIR/stderr"; then
	assert_stdout_contains udp-open
else
	assert_stdout_contains udp-denied
fi

# A profile that is named wins over the stored one.
run_fyai sandbox enter --profile dev -- sh -c "$UDP"
assert_status 0
assert_stdout_contains udp-open

# With a project the list follows the stored configuration: lockdown is in use.
run_fyai sandbox profiles --output yaml
assert_status 0
assert_stdout_contains 'current: true'
