#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the lockdown sandbox profile: sandbox_profile=lockdown confines the tools
# whatever `sandbox` says, with the secret locations of the platform and
# supported network operations denied by default. sandbox_lockdown changes
# only the keys that it names.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tool_diff.json
PORT="${MOCK_URL##*:}"
PORT="${PORT%%/*}"
HERE="$(pwd -P)"

mkdir -p "$HOME/.ssh" other
printf 'classified\n' >"$HOME/.ssh/id_test"
printf 'other-data\n' >other/data.txt
CONNECT="bash -c 'exec 3<>/dev/tcp/127.0.0.1/$PORT' && echo connected"
UDP_CONNECT="bash -c 'exec 3<>/dev/udp/127.0.0.1/$PORT' && echo udp-connected"

# shell_with YAML COMMAND: run a command in the shell tool under a configuration
# file, which does not change the stored configuration.
shell_with() {
	printf '%s\n' "$1" >"$TEST_DIR/lockdown.yaml"
	run_fyai --config "$TEST_DIR/lockdown.yaml" tool shell '{"command": "'"$2"'"}'
}

# The normal profile does not confine the tools: the key file and the network are open.
shell_with 'sandbox: false' "cat ~/.ssh/id_test; $CONNECT"
assert_status 0
assert_stdout_contains classified
assert_stdout_contains connected

# The lockdown profile enables the sandbox although `sandbox` is off, and denies ~/.ssh.
shell_with 'sandbox: false
sandbox_profile: lockdown' 'cat ~/.ssh/id_test || echo deny-ok'
assert_stdout_contains deny-ok
assert_stdout_not_contains classified

# The project stays readable.
shell_with 'sandbox_profile: lockdown' 'cat other/data.txt'
assert_stdout_contains other-data

# The network is denied where the kernel can restrict it (Landlock ABI 4, Linux 6.7).
RELEASE="$(uname -r)"
if [ "$(printf '%s\n6.7\n' "${RELEASE%%-*}" | sort -V | head -n 1)" = 6.7 ]; then
	shell_with 'sandbox_profile: lockdown' "$CONNECT || echo net-denied"
	assert_stdout_contains net-denied
	assert_stdout_not_contains connected
	shell_with 'sandbox_profile: lockdown' "$UDP_CONNECT || echo udp-denied"
	if grep -q 'UDP egress remains open' "$TEST_DIR/stderr"; then
		assert_stdout_contains udp-connected
	else
		assert_stdout_contains udp-denied
	fi
	# A port that the profile lists is open.
	shell_with "sandbox_profile: lockdown
sandbox_lockdown: { network: { tcp: { ports: [$PORT] } } }" "$CONNECT || echo net-denied"
	assert_stdout_contains connected
	shell_with "sandbox_profile: lockdown
sandbox_lockdown: { network: { tcp: { ports: [$PORT] } } }" "$UDP_CONNECT || echo udp-denied"
	if grep -q 'UDP egress remains open' "$TEST_DIR/stderr"; then
		assert_stdout_contains udp-connected
	else
		assert_stdout_contains udp-denied
	fi
fi

# A key that the profile names does not drop the default deny.
shell_with "sandbox_profile: lockdown
sandbox_lockdown: { allow: [{ path: $HERE/other, mode: ro }] }" 'cat ~/.ssh/id_test || echo deny-ok'
assert_stdout_contains deny-ok
assert_stdout_not_contains classified

# A deny of the profile replaces the default.
shell_with "sandbox_profile: lockdown
sandbox_lockdown: { deny: [$HERE/other] }" 'cat other/data.txt || echo deny-ok; cat ~/.ssh/id_test'
assert_stdout_contains deny-ok
assert_stdout_not_contains other-data
assert_stdout_contains classified

# A profile name that does not exist is refused.
shell_with 'sandbox_profile: paranoid' 'echo must-not-run'
assert_status_nonzero
assert_stdout_not_contains must-not-run

mock_stop 0
pass
