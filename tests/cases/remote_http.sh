#!/bin/bash
# SPDX-License-Identifier: MIT
# `fyai serve --listen` serves clients over HTTP and a WebSocket, behind a
# bearer token. --remote http://HOST:PORT/DIR gives what --remote . gives; a
# request without the token, or of another origin, path or form, is refused.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

REMOTE_SERVE_ARGS="--listen 127.0.0.1:0"
remote_test_setup claude_sdk_branch.json

PORT="$(sed -n 's#.*listening on http://127.0.0.1:\([0-9]*\)/.*#\1#p' "$TEST_DIR/server.err")"
[ -n "$PORT" ] || fail "the server did not say where it listens: $(cat "$TEST_DIR/server.err")"
URL="http://127.0.0.1:$PORT/."
[ "$(ls -l "$HOME/.fyai/serve.token" | cut -c1-10)" = "-rw-------" ] || fail "the token file is not private"

# A command gives what it gives over the socket.
"$FYAI_BIN" --remote . branch list >"$TEST_DIR/socket.out" </dev/null
"$FYAI_BIN" --remote "$URL" branch list >"$TEST_DIR/http.out" </dev/null
cmp "$TEST_DIR/socket.out" "$TEST_DIR/http.out"
"$FYAI_BIN" branch list --output json >"$TEST_DIR/local.json" </dev/null
"$FYAI_BIN" --remote "$URL" branch list --output json >"$TEST_DIR/http.json" </dev/null
cmp "$TEST_DIR/local.json" "$TEST_DIR/http.json"

# A failure keeps its cause and its exit status.
run_fyai --remote "$URL" config get nosuch/key
assert_status 1
assert_stderr_contains "key 'nosuch/key' not set"

# A prompt on standard input runs at the server.
set +e
printf 'Remote prompt.\n' | env -u MOCK_KEY "$FYAI_BIN" --remote "$URL" \
	>"$TEST_DIR/prompt.out" 2>"$TEST_DIR/prompt.err"
status=$?
set -e
[ "$status" -eq 0 ] || fail "the remote prompt failed: $(cat "$TEST_DIR/prompt.err")"
grep -q 'First answer.' "$TEST_DIR/prompt.out" ||
	fail "the remote prompt has no answer: $(cat "$TEST_DIR/prompt.out")"

# Another token is refused, with the cause.
set +e
FYAI_REMOTE_TOKEN="$(printf '0%.0s' $(seq 64))" "$FYAI_BIN" --remote "$URL" \
	branch list >"$TEST_DIR/bad.out" 2>"$TEST_DIR/bad.err" </dev/null
status=$?
set -e
[ "$status" -ne 0 ] || fail "a wrong token was accepted"
grep -q 'refused the token' "$TEST_DIR/bad.err" ||
	fail "the cause of the refusal: $(cat "$TEST_DIR/bad.err")"

# The requests of a client that is not fyai.
"$PYTHON" - "$PORT" "$HOME/.fyai/serve.token" "$FYAI_TIMEOUT_SCALE" <<'PY'
import base64, json, os, socket, struct, sys

port, token = int(sys.argv[1]), open(sys.argv[2]).read().strip()
scale = int(sys.argv[3])

def connect():
    s = socket.create_connection(('127.0.0.1', port))
    s.settimeout(10 * scale)
    return s

def head(s):
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    return data

def request(method='GET', path='/api/fyai/session', bearer=token, upgrade=True,
            origin=None, key=None, version='13', host=None):
    s = connect()
    lines = ['%s %s HTTP/1.1' % (method, path),
             'Host: %s' % (host or '127.0.0.1:%d' % port)]
    if upgrade:
        lines += ['Upgrade: websocket', 'Connection: Upgrade']
    lines.append('Sec-WebSocket-Version: %s' % version)
    lines.append('Sec-WebSocket-Key: %s' % (key or base64.b64encode(os.urandom(16)).decode()))
    if bearer is not None:
        lines.append('Authorization: Bearer %s' % bearer)
    if origin:
        lines.append('Origin: %s' % origin)
    s.sendall(('\r\n'.join(lines) + '\r\n\r\n').encode())
    return s, head(s)

def status(data):
    return int(data.split(b' ', 2)[1])

def refuse(want, what, **kw):
    s, data = request(**kw)
    assert status(data) == want, (what, data)
    s.close()

refuse(401, 'no token', bearer=None)
refuse(401, 'a wrong token', bearer='0' * 64)
refuse(401, 'a token of another length', bearer=token[:-1])
refuse(404, 'another path', path='/')
refuse(404, 'another path with a prefix', path='/api/fyai/sessions')
refuse(405, 'POST', method='POST')
refuse(426, 'no upgrade', upgrade=False)
refuse(426, 'another version', version='8')
refuse(403, 'another origin', origin='http://evil.example')
refuse(400, 'a key that is not 16 bytes', key='AAAA')

# The origin of the server is accepted.
s, data = request(origin='http://127.0.0.1:%d' % port)
assert status(data) == 101, data
s.close()

# A request that is not HTTP/1.1, and a head that is too long.
s = connect(); s.sendall(b'GET / HTTP/1.0\r\nHost: x\r\n\r\n')
assert status(head(s)) == 505; s.close()
s = connect(); s.sendall(b'GET / HTTP/1.1\r\nHost: x\nX: y\r\n\r\n')
assert status(head(s)) == 400; s.close()
s = connect(); s.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n' + b'X: ' + b'a' * 9000)
assert status(head(s)) == 431; s.close()

def send(s, opcode, payload, mask=True):
    key = os.urandom(4)
    n = len(payload)
    hdr = bytes([0x80 | opcode])
    flag = 0x80 if mask else 0
    if n < 126:
        hdr += bytes([flag | n])
    elif n < 65536:
        hdr += bytes([flag | 126]) + struct.pack('>H', n)
    else:
        hdr += bytes([flag | 127]) + struct.pack('>Q', n)
    if mask:
        hdr += key
        payload = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
    s.sendall(hdr + payload)

def recv_exact(s, n):
    data = b''
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data

def frame(s):
    b0, b1 = recv_exact(s, 2)
    n = b1 & 0x7f
    if n == 126:
        n = struct.unpack('>H', recv_exact(s, 2))[0]
    elif n == 127:
        n = struct.unpack('>Q', recv_exact(s, 8))[0]
    assert not b1 & 0x80, 'a server frame is not masked'
    return b0 & 0x0f, recv_exact(s, n)

def session():
    s, data = request()
    assert status(data) == 101, data
    return s

# fyai_hello over the WebSocket: the answer is text, and holds no image feature
# although the client offered one, because the lines carry no images.
s = session()
send(s, 1, json.dumps({'type': 'control_request', 'request_id': 'hello',
    'request': {'subtype': 'fyai_hello', 'version': 'test', 'protocol': 1,
                'features': ['project', 'image-le64', 'image-be64',
                             'image-le32', 'image-be32']}}).encode())
op, body = frame(s)
assert op == 1, op
answer = json.loads(body)
features = answer['response']['response']['features']
assert 'project' in features, answer
assert not [f for f in features if f.startswith('image')], features
send(s, 9, b'ping')
op, body = frame(s)
assert (op, body) == (10, b'ping'), (op, body)
send(s, 8, struct.pack('>H', 1000))
op, body = frame(s)
assert op == 8 and struct.unpack('>H', body[:2])[0] == 1000, (op, body)
s.close()

def closes_with(code, what, send_it):
    s = session()
    send_it(s)
    op, body = frame(s)
    assert op == 8 and struct.unpack('>H', body[:2])[0] == code, (what, op, body)
    s.close()

closes_with(1003, 'a binary message', lambda s: send(s, 2, b'\x00'))
closes_with(1002, 'a frame that is not masked', lambda s: send(s, 1, b'{}', mask=False))
closes_with(1007, 'a newline in a message', lambda s: send(s, 1, b'{"a":\n1}'))
PY

# A token file that others can read is not used, and the server says why.
printf '%s\n' "$(printf '1%.0s' $(seq 64))" >"$TEST_DIR/open.token"
chmod 644 "$TEST_DIR/open.token"
run_fyai serve --socket "$TEST_DIR/other.sock" --listen 127.0.0.1:0 --token-file "$TEST_DIR/open.token"
assert_status 1
assert_stderr_contains "must be a file that only you can read"
run_fyai serve --socket "$TEST_DIR/other.sock" --listen not-an-address
assert_status 1
assert_stderr_contains "is not HOST:PORT"

pass
