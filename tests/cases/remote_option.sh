#!/bin/bash
# SPDX-License-Identifier: MIT
# The global option --remote . runs a command, a prompt, the interactive
# session, or resume at `fyai serve` as a local run does: the same output, the
# same exit status, standard input, and the log of the server. `serve --daemon` runs in
# the background, and `serve --projects` serves each project under a directory.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

remote_test_setup claude_sdk_branch.json

# A command gives what it gives here, in each format.
"$FYAI_BIN" branch list >"$TEST_DIR/local.out" </dev/null
"$FYAI_BIN" --remote . branch list >"$TEST_DIR/remote.out" </dev/null
cmp "$TEST_DIR/local.out" "$TEST_DIR/remote.out"
"$FYAI_BIN" branch list --output json >"$TEST_DIR/local.json" </dev/null
"$FYAI_BIN" --remote . branch list --output json >"$TEST_DIR/remote.json" </dev/null
cmp "$TEST_DIR/local.json" "$TEST_DIR/remote.json"

# A failure keeps its cause and its exit status.
run_fyai --remote . config get nosuch/key
assert_status 1
assert_stderr_contains "key 'nosuch/key' not set"

# A prompt on standard input runs at the server.
set +e
printf 'Remote prompt.\n' | env -u MOCK_KEY "$FYAI_BIN" --remote . \
	>"$TEST_DIR/prompt.out" 2>"$TEST_DIR/prompt.err"
status=$?
set -e
[ "$status" -eq 0 ] || fail "the remote prompt failed: $(cat "$TEST_DIR/prompt.err")"
grep -q 'First answer.' "$TEST_DIR/prompt.out" ||
	fail "the remote prompt has no answer: $(cat "$TEST_DIR/prompt.out")"

# With no command line, a terminal gets the interactive session of the server.
"$FYAI_BIN" branch create remote/opt >"$TEST_DIR/branch.out"
pty_run "$TEST_DIR/session.out" "$(command -v env)" -u MOCK_KEY \
	"$FYAI_BIN" --remote . --branch remote/opt <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: mock-model
  - type: Interactive remote question.
  - wait-screen: Second answer.
  - type: /quit
PTY

# A fyai session starts with fyai_hello of the same protocol. A client of
# another protocol is refused, and a client that does not say hello is an SDK
# client: it runs no fyai request.
"$PYTHON" - "$TEST_DIR/.fyai/unix/remote.sock" "$FYAI_TIMEOUT_SCALE" <<'PY'
import json, socket, struct, sys
path, scale = sys.argv[1], int(sys.argv[2])
def ask(request):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(10 * scale)
    s.connect(path)
    s.sendall((json.dumps({'type': 'control_request', 'request_id': 'r',
                           'request': request}) + '\n').encode())
    data = b''
    while not data.endswith(b'\n'):
        chunk = s.recv(4096)
        assert chunk, 'the server closed without an answer'
        data += chunk
    s.close()
    return json.loads(data)['response']
r = ask({'subtype': 'fyai_hello', 'version': 'x', 'protocol': 999, 'features': []})
assert r['subtype'] == 'error' and 'protocol 999' in r['error'], r
r = ask({'subtype': 'fyai_run', 'args': ['branch', 'list']})
assert r['subtype'] == 'error' and 'needs a fyai session' in r['error'], r
r = ask({'subtype': 'fyai_hello', 'version': 'x', 'protocol': 1,
         'features': ['run', 'nosuch']})
assert r['subtype'] == 'success' and r['response']['session'] == 'fyai', r
assert r['response']['features'] == ['run'], r
# Images go between ends of the same byte order and word size: the feature
# names both, and an end of another kind is never offered images.
mine = 'image-%s%d' % ('le' if sys.byteorder == 'little' else 'be',
                       8 * struct.calcsize('P'))
other = 'image-%s%d' % ('be' if sys.byteorder == 'little' else 'le',
                        8 * struct.calcsize('P'))
for odd in (other, 'image-le16', 'image'):
    r = ask({'subtype': 'fyai_hello', 'version': 'x', 'protocol': 1,
             'features': ['run', odd]})
    assert r['response']['features'] == ['run'], (odd, r)
r = ask({'subtype': 'fyai_hello', 'version': 'x', 'protocol': 1,
         'features': ['run', mine]})
assert r['response']['features'] in (['run'], ['run', mine]), r
PY

# resume is interactive: the server selects the session, here by its name and
# as the session updated last.
pty_run "$TEST_DIR/resume.out" "$(command -v env)" -u MOCK_KEY \
	"$FYAI_BIN" --remote . resume remote/opt <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: Second answer.
  - type: /quit
PTY
pty_run "$TEST_DIR/resume-last.out" "$(command -v env)" -u MOCK_KEY \
	"$FYAI_BIN" --remote . resume --last --all <<'PTY'
rows: 40
cols: 100
steps:
  - wait-screen: Second answer.
  - type: /quit
PTY
grep -q "resume selects 'remote/opt'" "$TEST_DIR/server.err" ||
	fail "the server did not select the last session: $(cat "$TEST_DIR/server.err")"

remote_test_stop
mock_stop 2
assert_request 0 'any("Remote prompt." in str(m.get("content", "")) for m in r["body"]["messages"])'
assert_request 1 'any("Interactive remote question." in str(m.get("content", "")) for m in r["body"]["messages"])'
grep -q 'client [0-9]*: fyai .*, protocol 1' "$TEST_DIR/server.err" ||
	fail "the server did not log the hello: $(cat "$TEST_DIR/server.err")"
grep -q 'client 1: connected' "$TEST_DIR/server.err" ||
	fail "the server did not log the client: $(cat "$TEST_DIR/server.err")"
grep -q 'started the command [0-9]*: branch' "$TEST_DIR/server.err" ||
	fail "the server did not log the command: $(cat "$TEST_DIR/server.err")"
grep -q 'the command [0-9]* exited with status 1' "$TEST_DIR/server.err" ||
	fail "the server did not log the failure: $(cat "$TEST_DIR/server.err")"

# A daemon reports the server it started and serves until it is stopped.
run_fyai serve --daemon
assert_status 0
assert_stdout_contains 'serve: started the server'
assert_stdout_contains '.fyai/logs/serve.log'
run_fyai serve status
assert_status 0
assert_stdout_contains "project: $TEST_DIR"
run_fyai serve stop
assert_status 0
assert_stdout_contains 'serve: stopped the server'
grep -q 'stop requested' "$TEST_DIR/.fyai/logs/serve.log" ||
	fail "the daemon did not log the stop: $(cat "$TEST_DIR/.fyai/logs/serve.log")"

# A server of projects starts a project server for the project a client names.
mkdir -p "$TEST_DIR/projects/one/.git"
(cd "$TEST_DIR/projects/one" && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" config set model one-model >/dev/null 2>&1)
run_fyai serve --projects "$TEST_DIR/projects" --daemon
assert_status 0
assert_stdout_contains '.fyai/unix/serve.sock'
run_fyai --remote . --project one config get model
assert_status 0
assert_stdout_contains one-model
run_fyai --remote . --project ../ config get model
assert_status 1
assert_stderr_contains 'not under the projects of the server'
run_fyai serve status --projects
assert_status 0
assert_stdout_contains "root: $TEST_DIR/projects/one"
run_fyai serve stop --projects
assert_status 0
grep -q 'started the project server' "$HOME/.fyai/logs/serve.log" ||
	fail "the server of projects did not log the project: $(cat "$HOME/.fyai/logs/serve.log")"
pass
