#!/bin/bash
# SPDX-License-Identifier: MIT
# `serve --once` keeps a session whose client left until its turn ends, lists
# it in its record while it runs, and stops with the turn.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_once.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" <<'PY'
import json, os, select, subprocess, sys, time
binary, cwd, scale = sys.argv[1:]
scale = int(scale)
sock = cwd + '/.fyai/unix/remote.sock'
record = sock + '.live'
log = open(cwd + '/server.err', 'wb')
server = subprocess.Popen([binary, 'serve', '--once'], cwd=cwd,
                          stdout=subprocess.PIPE, stderr=log)
def until(test, what):
    deadline = time.monotonic() + 20 * scale
    while not test():
        assert server.poll() is None or what == 'exit', what + ': ' + open(cwd + '/server.err').read()
        assert time.monotonic() < deadline, what
        select.select([], [], [], 0.05)
until(lambda: os.path.exists(sock), 'socket')
client = subprocess.Popen([binary, '-b', 'bg/one', 'remote', 'connect',
                           '--protocol', 'sdk'], cwd=cwd, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def send(value):
    os.write(client.stdin.fileno(), (json.dumps(value) + '\n').encode())
send({'type': 'control_request', 'request_id': 'init',
      'request': {'subtype': 'initialize'}})
send({'type': 'user', 'message': {'role': 'user',
                                  'content': 'Work without a client.'}})
def live():
    try:
        return json.load(open(record))['sessions']
    except (OSError, ValueError, KeyError):
        return []
until(lambda: any(s['branch'] == 'bg/one' and s['busy'] for s in live()),
      'a busy session in the record')
# The client leaves while the model request is held.
client.terminate()
client.communicate(timeout=10)
until(lambda: not any(s['attached'] for s in live()) and live(), 'detached')
assert server.poll() is None, 'the server stopped with a turn running'
open(cwd + '/release-once', 'w').close()
server.wait(timeout=20 * scale)
assert server.returncode == 0, open(cwd + '/server.err').read()
assert not os.path.exists(sock) and not os.path.exists(record), 'files left'
PY
"$FYAI_BIN" -b bg/one export -o saved.md >/dev/null 2>&1 || fail "no export of bg/one"
grep -q 'Finished in the background.' saved.md || fail "the turn was not stored"
