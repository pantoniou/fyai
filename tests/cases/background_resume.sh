#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that `fyai resume` lists server-hosted live sessions and selecting one
# attaches to it.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_once.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
"$FYAI_BIN" serve --once --daemon >/dev/null 2>&1 || fail "the server did not start"
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" <<'PY'
import json, os, select, subprocess, sys, time
binary, cwd, scale = sys.argv[1:]
record = cwd + '/.fyai/unix/remote.sock.live'
client = subprocess.Popen([binary, '-b', 'bg/live', 'remote', 'connect',
                           '--protocol', 'sdk'], cwd=cwd, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def send(value):
    os.write(client.stdin.fileno(), (json.dumps(value) + '\n').encode())
send({'type': 'control_request', 'request_id': 'init',
      'request': {'subtype': 'initialize'}})
send({'type': 'user', 'message': {'role': 'user',
                                  'content': 'Work without a client.'}})
def sessions():
    try:
        return json.load(open(record))['sessions']
    except (OSError, ValueError, KeyError):
        return []
deadline = time.monotonic() + 20 * int(scale)
while not any(s['branch'] == 'bg/live' and s['busy'] for s in sessions()):
    assert time.monotonic() < deadline, 'no busy session'
    select.select([], [], [], 0.05)
client.terminate()
client.communicate(timeout=10)
deadline = time.monotonic() + 20 * int(scale)
while any(s['attached'] for s in sessions()):
    assert time.monotonic() < deadline, 'the client stayed attached'
    select.select([], [], [], 0.05)
PY

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" resume <<'PTY'
ready: toggle foreign sessions
steps:
  - wait-screen: "[live · running]"
  - write: "/live\r"
  - wait-screen: "Enter resume"
  - frame: 2
  - write: "\r"
  - wait-gone: "toggle foreign sessions"
  - wait-screen: "…"
  - touch: "${TEST_DIR}/release-once"
  - wait-screen: "Finished in the background."
    timeout: session
PTY
pass
