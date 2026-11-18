#!/bin/bash
# SPDX-License-Identifier: MIT
# A client of one live session opens the picker, joins another live session,
# and leaves it with /detach; the server stops when both turns ended.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_switch.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
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
for branch, word in (('x/one', 'Work one'), ('x/two', 'Work two')):
    client = subprocess.Popen([binary, '-b', branch, 'remote', 'connect',
                               '--protocol', 'sdk'], cwd=cwd,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
    for value in ({'type': 'control_request', 'request_id': 'init',
                   'request': {'subtype': 'initialize'}},
                  {'type': 'user', 'message': {'role': 'user',
                                               'content': word}}):
        os.write(client.stdin.fileno(), (json.dumps(value) + '\n').encode())
    until(lambda: any(s['branch'] == branch and s['busy'] for s in sessions()),
          'no busy ' + branch)
    client.terminate()
    client.communicate(timeout=10)
until(lambda: len(sessions()) == 2 and not any(s['attached'] for s in sessions()),
      'both sessions detached')
PY

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b x/one remote connect <<'PTY'
ready: "…"
steps:
  - type: /resume
  - wait-screen: "[live · running]"
  - write: "/two\r"
  - wait-screen: "Enter resume"
  - frame: 2
  - write: "\r"
  - wait-gone: "toggle foreign sessions"
  - wait-screen: "…"
  - touch: "${TEST_DIR}/release-two"
  - wait-screen: "Finished two."
    timeout: session
  - type: /detach
PTY

touch release-one
log=.fyai/logs/serve.log
pid="$(sed -n 's/.*serve\[\([0-9]*\)\]: serving.*/\1/p' "$log" | head -1)"
"$PYTHON" "$(dirname "$0")/../wait_exit.py" "$pid" "$((20 * FYAI_TIMEOUT_SCALE))" ||
    fail "the server did not stop when both turns ended"
mock_stop 2
"$FYAI_BIN" -b x/one export -o one.md >/dev/null 2>&1 || fail "no export of x/one"
grep -q 'Finished one.' one.md || fail "x/one did not finish"
pass
