#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that an interactive session auto-registers with a running project
# server, allows watcher attachment while refusing a second driver, and reuses
# the registration connection to park on /background.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_park.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1

"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

# Start the interactive session and signal ready once idle so checks can run.
SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/ready"
pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<PTY &
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - touch: "$SYNC/ready"
    timeout: session
  - wait-file: "$SYNC/checked"
    equals: ok
    timeout: session
  - type: /background
  - wait: "is parked at a server"
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/ready" || true

status() {
	"$FYAI_BIN" serve status --output json
}
status >"$TEST_DIR/status-driven.json" ||
	fail "serve status failed"
"$PYTHON" - "$TEST_DIR/status-driven.json" <<'PY' ||
import json, sys
s = json.load(open(sys.argv[1]))['sessions']
assert [(x['branch'], x['attached'], x['local'], x['busy']) for x in s] == \
    [('main', True, True, False)], s
PY
	fail "the server does not list the session that its client drives: $(cat "$TEST_DIR/status-driven.json")"

# Second driver must be rejected; a watcher can attach and receive a snapshot.
"$PYTHON" - "$TEST_DIR/.fyai/unix/remote.sock" "$FYAI_TIMEOUT_SCALE" <<'PY' ||
import json, os, select, socket, sys, time
path, scale = sys.argv[1], int(sys.argv[2])

def receive(s, timeout=20):
    data = bytearray()
    end = time.monotonic() + timeout * scale
    while time.monotonic() < end:
        assert select.select([s], [], [], max(0, end - time.monotonic()))[0], 'frame timeout'
        c = s.recv(1)
        assert c, 'unexpected EOF'
        if c == b'\n':
            return json.loads(data)
        data.extend(c)
    raise AssertionError('frame timeout')

def send(s, value):
    s.sendall((json.dumps(value) + '\n').encode())

def raw():
    s = socket.socket(socket.AF_UNIX)
    s.connect(path)
    send(s, {'type':'control_request','request_id':'hello','request':{
        'subtype':'fyai_hello','version':'x','protocol':1,'features':['model']}})
    assert receive(s)['response']['subtype'] == 'success'
    return s

def request(s, rid, **fields):
    send(s, {'type':'control_request','request_id':rid,'request':fields})
    while True:
        f = receive(s)
        if f['type'] == 'control_response' and f['response']['request_id'] == rid:
            return f['response']

s = raw()
r = request(s, 'drive', subtype='fyai_session_attach', branch='main')
assert r['subtype'] == 'error' and 'has a client' in r['error'], r
s.close()

s = raw()
r = request(s, 'w', subtype='fyai_session_attach', branch='main', watch=True)
assert r['subtype'] == 'success', r
r = request(s, 'init', subtype='initialize', fyai={'model':1})
assert r['subtype'] == 'success', r
seen, texts = [], ''
while True:
    f = receive(s)
    if f.get('subtype') == 'snapshot':
        assert f['request_id'] == 'init', f
        seen.append(f['phase'])
        if f['phase'] == 'end':
            break
    else:
        texts += json.dumps(f)
assert seen == ['begin', 'end'], seen
assert '"branch": "main"' in texts, 'the snapshot is not of the session: ' + texts[:2000]
s.close()
PY
	fail "a watcher cannot read the registered session"

echo ok >"$SYNC/checked"
wait "$driver" || fail "the session did not go to the server: $(tail -c 2000 "$TEST_DIR/pty.out")"

status >"$TEST_DIR/status-parked.json" || fail "serve status failed"
"$PYTHON" - "$TEST_DIR/status-parked.json" <<'PY' ||
import json, sys
s = json.load(open(sys.argv[1]))['sessions']
assert [(x['branch'], x['attached'], x['local']) for x in s] == \
    [('main', False, False)], s
PY
	fail "the server did not keep the session: $(cat "$TEST_DIR/status-parked.json")"
grep -q "registered the worker" .fyai/logs/serve.log ||
	fail "the server did not log the registration: $(cat .fyai/logs/serve.log)"
grep -q "its client left; the server keeps the session" .fyai/logs/serve.log ||
	fail "the session did not move on its registration"
! grep -q "adopted the worker" .fyai/logs/serve.log ||
	fail "/background made a second connection"
pass
