#!/bin/bash
# SPDX-License-Identifier: MIT
# --remote ssh://HOST/DIR reaches `fyai serve` through `ssh HOST fyai remote
# bridge`: the same output and exit status as --remote . A fake ssh stands in
# for ssh: it drops the ssh options and the host, and runs the bridge here.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

remote_test_setup claude_sdk_branch.json

# The environment of ssh holds no variable of fyai, so the paths are written in.
cat >"$TEST_DIR/fake-ssh" <<SH
#!/bin/sh
# fake-ssh [-T] [-p PORT] -- HOST fyai remote bridge
printf '%s\\n' "\$*" >>"$TEST_DIR/ssh.log"
while [ "\$1" != "--" ]; do shift; done
shift 3
# What a login script writes before the bridge, one case at a time.
printf 'Welcome to the host\n'
printf 'FYAI-BRIDGE 0000\n'
printf 'a prompt without its end'
if [ -f "$TEST_DIR/no-bridge" ]; then exit 0; fi
if [ -f "$TEST_DIR/flood" ]; then head -c 70000 /dev/zero | tr '\\0' x; exit 0; fi
exec "$FYAI_BIN" "\$@"
SH
chmod +x "$TEST_DIR/fake-ssh"
export FYAI_SSH="$TEST_DIR/fake-ssh"
FAKE_SSH_LOG="$TEST_DIR/ssh.log"
: >"$FAKE_SSH_LOG"

# A command gives what it gives over --remote . and what it gives here.
"$FYAI_BIN" --remote . branch list >"$TEST_DIR/socket.out" </dev/null
"$FYAI_BIN" --remote ssh://user@host:2222/. branch list \
	>"$TEST_DIR/ssh.out" </dev/null
cmp "$TEST_DIR/socket.out" "$TEST_DIR/ssh.out"
grep -Eqx -- '-T -p 2222 -- user@host fyai remote bridge --nonce [0-9a-f]{32}' "$FAKE_SSH_LOG" ||
	fail "ssh got another command line: $(cat "$FAKE_SSH_LOG")"

# A failure keeps its cause and its exit status.
run_fyai --remote ssh://host/. config get nosuch/key
assert_status 1
assert_stderr_contains "key 'nosuch/key' not set"

# A host that ssh would read as an option is not an ssh target.
run_fyai --remote ssh://-oProxyCommand=x/. branch list
assert_status 1
assert_stderr_contains "is not an ssh target"
[ "$(wc -l <"$FAKE_SSH_LOG")" -eq 2 ] || fail "ssh ran for a bad host"

# ssh failing to reach the host is an error, not a hang.
FYAI_SSH=false run_fyai --remote ssh://host/. branch list
assert_status 1

# The output of a login script before the bridge is skipped: the fake writes a
# banner, a line that looks like the start of the bridge, and an unended line.
# The commands above already ran through it.

# ssh that ends before the bridge: the cause has what it wrote.
touch "$TEST_DIR/no-bridge"
run_fyai --remote ssh://host/. branch list
assert_status 1
assert_stderr_contains "ssh ended before the bridge started"
assert_stderr_contains "Welcome to the host"
rm "$TEST_DIR/no-bridge"

# ssh that writes without end gives up at the limit.
touch "$TEST_DIR/flood"
run_fyai --remote ssh://host/. branch list
assert_status 1
assert_stderr_contains "before the bridge: not an fyai bridge"
rm "$TEST_DIR/flood"

pass
