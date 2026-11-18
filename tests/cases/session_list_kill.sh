#!/bin/bash
# SPDX-License-Identifier: MIT
# `session list` shows the live sessions of the project with their state, and
# `session kill` ends one at its server, which stops when none is left.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_once.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1

run_fyai session list
assert_status 0
assert_stdout_contains "no live sessions"

"$FYAI_BIN" serve --once --daemon >/dev/null 2>&1 || fail "the server did not start"
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" <<'PY'
import json, os, select, subprocess, sys, time
binary, cwd, scale = sys.argv[1:]
record = cwd + '/.fyai/unix/remote.sock.live'
def sessions():
    try:
        return json.load(open(record))['sessions']
    except (OSError, ValueError, KeyError):
        return []
def until(test, what):
    deadline = time.monotonic() + 20 * int(scale)
    while not test():
        assert time.monotonic() < deadline, what
        select.select([], [], [], 0.05)
client = subprocess.Popen([binary, '-b', 'bg/run', 'remote', 'connect',
                           '--protocol', 'sdk'], cwd=cwd, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
for value in ({'type': 'control_request', 'request_id': 'init',
               'request': {'subtype': 'initialize'}},
              {'type': 'user', 'message': {'role': 'user',
                                           'content': 'Work without a client.'}}):
    os.write(client.stdin.fileno(), (json.dumps(value) + '\n').encode())
until(lambda: any(s['branch'] == 'bg/run' and s['busy'] for s in sessions()),
      'no busy session')
client.terminate()
client.communicate(timeout=10)
until(lambda: not any(s['attached'] for s in sessions()), 'still attached')
PY

run_fyai session list --output json
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY' || fail "session list: $(cat "$TEST_DIR/stdout")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert [(r['branch'], r['state'], r['attached']) for r in rows] == [('bg/run', 'running', False)], rows
assert rows[0]['pid'] > 0, rows
PY
run_fyai session ps
assert_stdout_contains "bg/run"

# A session of a terminal lists them too.
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: /session list
  - wait-screen: "bg/run"
    timeout: session
  - wait-screen: "running"
PTY

run_fyai session kill no/such
[ "$FYAI_STATUS" -ne 0 ] || fail "killed a session that is not live"
run_fyai session kill bg/run
assert_status 0
assert_stdout_contains "ended bg/run"

log=.fyai/logs/serve.log
pid="$(sed -n 's/.*serve\[\([0-9]*\)\]: serving.*/\1/p' "$log" | head -1)"
"$PYTHON" "$(dirname "$0")/../wait_exit.py" "$pid" "$((20 * FYAI_TIMEOUT_SCALE))" ||
    fail "the server did not stop with its last session"
run_fyai session list
assert_stdout_contains "no live sessions"
pass
