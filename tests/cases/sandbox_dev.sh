#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the dev sandbox profile: sandbox_profile=dev confines the tools to
# project work. The project and the toolchains are
# open, the secret locations are denied, PATH keeps only the directories that
# the tools reach, and TCP connects only to the ports of the profile.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

# The sandbox grants the system scratch trees whole: keep the project out of them, or a
# host without namespaces cannot protect the arena.
FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
mock_start tool_diff.json
PORT="${MOCK_URL##*:}"
PORT="${PORT%%/*}"
HERE="$(pwd -P)"

mkdir -p "$HOME/.ssh" "$HOME/.cargo/bin" "$HOME/bin" other
printf 'classified\n' >"$HOME/.ssh/id_test"
printf 'other-data\n' >other/data.txt
printf '#!/bin/sh\necho cargo-tool\n' >"$HOME/.cargo/bin/devtool"
printf '#!/bin/sh\necho home-tool\n' >"$HOME/bin/hometool"
chmod +x "$HOME/.cargo/bin/devtool" "$HOME/bin/hometool"
CONNECT="bash -c 'exec 3<>/dev/tcp/127.0.0.1/$PORT' && echo connected"

# shell_with YAML COMMAND: run a command in the shell tool under a configuration
# file, which does not change the stored configuration.
shell_with() {
	printf '%s\n' "$1" >"$TEST_DIR/dev.yaml"
	PATH="$HOME/bin:$HOME/.cargo/bin:$PATH" \
		run_fyai --config "$TEST_DIR/dev.yaml" tool shell '{"command": "'"$2"'"}'
}

# A new arena stores the profiles of the sample, so the configuration lists them.
mkdir fresh
(cd fresh && "$FYAI_BIN" init >/dev/null 2>&1 && "$FYAI_BIN" -k test-key config get sandbox_profiles/dev/network \
	>"$TEST_DIR/profile.out" 2>&1) || fail 'no fresh arena'
grep -q 9418 "$TEST_DIR/profile.out" || fail 'the dev profile is not in the stored configuration'
(cd fresh && "$FYAI_BIN" -k test-key config get sandbox_profiles/lockdown/view >"$TEST_DIR/profile.out" 2>&1)
grep -q true "$TEST_DIR/profile.out" || fail 'the lockdown profile is not in the stored configuration'

(cd fresh && "$FYAI_BIN" -k test-key config get sandbox_profiles/build >"$TEST_DIR/profile.out" 2>&1)
grep -q 'extends: dev' "$TEST_DIR/profile.out" && grep -q 'userns' "$TEST_DIR/profile.out" ||
	fail 'the build profile is not in the stored configuration'

(cd fresh && "$FYAI_BIN" -k test-key config get sandbox_profiles/yocto >"$TEST_DIR/profile.out" 2>&1)
grep -q 'extends: build' "$TEST_DIR/profile.out" && grep -q 'private: true' "$TEST_DIR/profile.out" ||
	fail 'the yocto profile is not in the stored configuration'

(cd fresh && "$FYAI_BIN" -k test-key config get sandbox_profiles/dev-view >"$TEST_DIR/profile.out" 2>&1)
grep -q 'extends: dev' "$TEST_DIR/profile.out" && grep -q 'view: true' "$TEST_DIR/profile.out" ||
	fail 'the dev-view profile is not in the stored configuration'

# The dev profile confines the tools.
shell_with 'sandbox_profile: dev' 'cat ~/.ssh/id_test || echo deny-ok'
assert_stdout_contains deny-ok
assert_stdout_not_contains classified

# The dev profile lets a program open and list /, and a profile without list_root does not.
shell_with 'sandbox_profile: dev' 'ls / >/dev/null && echo root-listed'
assert_stdout_contains root-listed
shell_with 'sandbox_profile: nolist
sandbox_profiles:
  nolist:
    extends: dev
    list_root: false' 'ls / >/dev/null 2>&1 && echo root-listed || echo root-denied'
assert_stdout_contains root-denied

# The project is readable and writable.
shell_with 'sandbox_profile: dev' 'cat other/data.txt && echo w > other/made.txt && cat other/made.txt'
assert_stdout_contains other-data
[ -f other/made.txt ] || fail 'a tool could not create a file in the project'

# Where the host can make user, mount and PID namespaces, the arena is covered by an
# empty mount in the tool, and the tool creates entries in the project root. Without
# them, Landlock denies the arena, and the directory above it keeps only the right to
# list, so the root cannot take new entries.
shell_with 'sandbox_profile: dev' 'touch cover-probe && rm -f cover-probe && echo cover-works'
if grep -q cover-works "$TEST_DIR/stdout"; then
	shell_with 'sandbox_profile: dev' 'cat other/data.txt && echo w > made.txt && cat made.txt'
	[ -f made.txt ] || fail 'a tool could not create a file in the project root'
	shell_with 'sandbox_profile: dev' 'mkdir madedir && touch madedir/x && echo dir-ok'
	assert_stdout_contains dir-ok
	[ -d madedir ] || fail 'a tool could not create a directory in the project root'

	shell_with 'sandbox_profile: dev' 'test -z $(ls -A .fyai) && echo arena-hidden'
	assert_stdout_contains arena-hidden
	# The tool is in a PID namespace of its own: it has a small process id under an init,
	# and the procfs lists its own processes, not those of the host.
	shell_with 'sandbox_profile: dev' 'echo pid=$$; echo procs=$(ls /proc | grep -c ^[0-9])'
	pid="$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' "$TEST_DIR/stdout" | head -n 1)"
	[ -n "$pid" ] && [ "$pid" -lt 10 ] || fail "the tool has process id $pid, not one of a PID namespace"
	procs="$(sed -n 's/.*procs=\([0-9]*\).*/\1/p' "$TEST_DIR/stdout" | head -n 1)"
	[ -n "$procs" ] && [ "$procs" -lt 10 ] || fail "the tool sees $procs processes"
	# The root of the parent process does not show it either.
	shell_with 'sandbox_profile: dev' 'test -z $(ls -A /proc/$PPID/root/$PWD/.fyai 2>/dev/null) && echo proc-hidden'
	assert_stdout_contains proc-hidden
fi

# The arena cannot be written, with the cover or without it, and the host keeps it.
shell_with 'sandbox_profile: dev' 'echo x > .fyai/probe || echo arena-readonly'
assert_stdout_contains arena-readonly
[ ! -e .fyai/probe ] || fail 'a tool wrote into the arena'
[ -d .fyai/objects ] || [ -n "$(ls -A .fyai)" ] || fail 'the host lost its arena'

# A toolchain that the profile grants runs from PATH.
shell_with 'sandbox_profile: dev' 'devtool'
assert_stdout_contains cargo-tool

# A directory that the profile denies leaves PATH, and its programs are not found.
shell_with "sandbox_profile: t
sandbox_profiles: { t: { extends: dev, deny: [$HOME/bin] } }" 'echo $PATH; hometool || echo no-hometool'
assert_stdout_contains no-hometool
assert_stdout_not_contains "$HOME/bin"
assert_stdout_contains "$HOME/.cargo/bin"

# The user adds a grant to the toolchains: the default grants stay.
shell_with "sandbox_profile: t
sandbox_profiles: { t: { extends: dev, allow: [{ path: $HOME/bin, mode: ro }] } }" 'hometool; devtool'
assert_stdout_contains home-tool
assert_stdout_contains cargo-tool

# TCP connects only to the listed ports, where the kernel can restrict it.
RELEASE="$(uname -r)"
if [ "$(printf '%s\n6.7\n' "${RELEASE%%-*}" | sort -V | head -n 1)" = 6.7 ]; then
	shell_with 'sandbox_profile: dev' "$CONNECT || echo net-denied"
	assert_stdout_contains net-denied
	assert_stdout_not_contains connected
	# dev-view confines the tools as dev does; it differs by the view alone.
	shell_with 'sandbox_profile: dev-view' "$CONNECT || echo net-denied"
	assert_stdout_contains net-denied
	assert_stdout_not_contains connected
	shell_with "sandbox_profile: t
sandbox_profiles: { t: { extends: dev, network: { tcp: { ports: [$PORT] } } } }" "$CONNECT || echo net-denied"
	assert_stdout_contains connected
fi

# The warning that egress stays open is one line for the process.
shell_with 'sandbox_profile: dev' 'true'
[ "$(grep -c 'egress remains open' "$TEST_DIR/stderr" || true)" -le 2 ] ||
	fail "the egress warning repeats"

# A profile of the user extends a built-in one, and may name the host.
shell_with "sandbox_profile: mine
sandbox_profiles:
  mine:
    extends: dev
    allow: [{ path: '{{host_specific}}', mode: ro }, '$HOME/bin']" 'hometool; devtool'
assert_stdout_contains home-tool
assert_stdout_contains cargo-tool

# A profile that is not defined, a cycle and a placeholder that is unknown are refused.
shell_with 'sandbox_profile: paranoid' 'echo must-not-run'
assert_status_nonzero
assert_stdout_not_contains must-not-run
shell_with "sandbox_profile: a
sandbox_profiles: { a: { extends: b }, b: { extends: a } }" 'echo must-not-run'
assert_status_nonzero
assert_stdout_not_contains must-not-run
shell_with "sandbox_profile: a
sandbox_profiles: { a: { allow: ['{{nothing}}'] } }" 'echo must-not-run'
assert_status_nonzero
assert_stdout_not_contains must-not-run

mock_stop 0
pass
