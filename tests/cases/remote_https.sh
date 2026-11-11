#!/bin/bash
# SPDX-License-Identifier: MIT
# `fyai serve --listen https://ADDRESS` serves the clients of remote_http.sh over
# TLS. The client trusts a pinned key, or a CA that names the host; a key that is
# not pinned, a CA that is not trusted and a name that does not match are
# refused with their cause.
set -eu
. "$(dirname "$0")/../remote-harness.sh"

REMOTE_SERVE_ARGS="--listen https://127.0.0.1:0 --listen 127.0.0.1:0"
remote_test_setup claude_sdk_branch.json

PORT="$(sed -n 's#.*listening on https://127.0.0.1:\([0-9]*\)/.*#\1#p' "$TEST_DIR/server.err")"
[ -n "$PORT" ] || fail "the server did not say where it listens: $(cat "$TEST_DIR/server.err")"
PIN="$(sed -n 's#.*the key of the server is \(sha256:[0-9a-f]*\).*#\1#p' "$TEST_DIR/server.err")"
[ "${#PIN}" -eq 71 ] || fail "the log has no key: $(cat "$TEST_DIR/server.err")"
URL="https://127.0.0.1:$PORT/."
[ "$(ls -l "$HOME/.fyai/tls/server.pem" | cut -c1-10)" = "-rw-------" ] || fail "the key file is not private"
! grep -q 'warning: plain http' "$TEST_DIR/server.err" || fail "a loopback listener warned"

# The fingerprint command says what the log says.
run_fyai remote fingerprint
assert_status 0
assert_stdout_contains "$PIN"

# A command gives what it gives over the socket.
"$FYAI_BIN" --remote . branch list >"$TEST_DIR/socket.out" </dev/null
FYAI_REMOTE_PIN="$PIN" "$FYAI_BIN" --remote "$URL" branch list >"$TEST_DIR/tls.out" </dev/null
cmp "$TEST_DIR/socket.out" "$TEST_DIR/tls.out"

# A failure keeps its cause and its exit status.
export FYAI_REMOTE_PIN="$PIN"
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

# The state of the branch moved with the prompt: compare with the socket again.
same_as_socket() {
	"$FYAI_BIN" --remote . branch list >"$TEST_DIR/socket.out" </dev/null
	cmp "$TEST_DIR/socket.out" "$1"
}

# The key and the roots decide. Each refusal names its cause.
expect_refused() {
	local what="$1" cause="$2"
	shift 2
	set +e
	env "$@" "$FYAI_BIN" --remote "$URL" branch list \
		>"$TEST_DIR/refused.out" 2>"$TEST_DIR/refused.err" </dev/null
	status=$?
	set -e
	[ "$status" -ne 0 ] || fail "$what was accepted"
	grep -q -- "$cause" "$TEST_DIR/refused.err" ||
		fail "$what: expected '$cause': $(cat "$TEST_DIR/refused.err")"
}
expect_refused "a key that is not pinned" "not trusted: self-signed certificate; its key is $PIN" \
	-u FYAI_REMOTE_PIN
expect_refused "another pin" "is $PIN, not the pinned sha256:$(printf 'a%.0s' $(seq 64))" \
	FYAI_REMOTE_PIN="sha256:$(printf 'a%.0s' $(seq 64))"
expect_refused "a pin of the wrong size" "the pin must be sha256: and 64 hexadecimal digits" \
	FYAI_REMOTE_PIN="sha256:abc"
expect_refused "a token that is not the token of the server" "refused the token" \
	FYAI_REMOTE_TOKEN="$(printf '0%.0s' $(seq 64))"
# Colons and capitals are the same pin.
UPPER="sha256:$(printf '%s' "${PIN#sha256:}" | tr a-f A-F | sed 's/../&:/g; s/:$//')"
FYAI_REMOTE_PIN="$UPPER" "$FYAI_BIN" --remote "$URL" branch list >"$TEST_DIR/upper.out" </dev/null
same_as_socket "$TEST_DIR/upper.out"

# The requests of a client that is not fyai.
"$PYTHON" - "$PORT" "$HOME/.fyai/serve.token" "$FYAI_TIMEOUT_SCALE" <<'PY'
import base64, json, os, socket, ssl, struct, sys

port, token = int(sys.argv[1]), open(sys.argv[2]).read().strip()
scale = int(sys.argv[3])
context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE

def connect(alpn=None):
    ctx = context
    if alpn:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.set_alpn_protocols(alpn)
    raw = socket.create_connection(('127.0.0.1', port))
    raw.settimeout(10 * scale)
    return ctx.wrap_socket(raw, server_hostname='127.0.0.1')

def head(s):
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    return data

def request(bearer=token, origin=None, host=None):
    s = connect()
    lines = ['GET /api/fyai/session HTTP/1.1', 'Host: %s' % (host or '127.0.0.1:%d' % port),
             'Upgrade: websocket', 'Connection: Upgrade', 'Sec-WebSocket-Version: 13',
             'Sec-WebSocket-Key: %s' % base64.b64encode(os.urandom(16)).decode()]
    if bearer is not None:
        lines.append('Authorization: Bearer %s' % bearer)
    if origin:
        lines.append('Origin: %s' % origin)
    s.sendall(('\r\n'.join(lines) + '\r\n\r\n').encode())
    return s, head(s)

def status(data):
    return int(data.split(b' ', 2)[1])

s, data = request(bearer=None); assert status(data) == 401, data; s.close()
s, data = request(bearer='0' * 64); assert status(data) == 401, data; s.close()
# The origin of a TLS server is https, not http.
s, data = request(origin='http://127.0.0.1:%d' % port); assert status(data) == 403, data; s.close()
s, data = request(origin='https://127.0.0.1:%d' % port); assert status(data) == 101, data; s.close()

def send(s, opcode, payload):
    key = os.urandom(4)
    n = len(payload)
    hdr = bytes([0x80 | opcode])
    if n < 126:
        hdr += bytes([0x80 | n])
    elif n < 65536:
        hdr += bytes([0x80 | 126]) + struct.pack('>H', n)
    else:
        hdr += bytes([0x80 | 127]) + struct.pack('>Q', n)
    s.sendall(hdr + key + bytes(b ^ key[i % 4] for i, b in enumerate(payload)))

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
    return b0 & 0x0f, recv_exact(s, n)

# A hello of 600 KB, which takes many TLS records each way.
s, data = request(); assert status(data) == 101, data
send(s, 1, json.dumps({'type': 'control_request', 'request_id': 'hello',
    'request': {'subtype': 'fyai_hello', 'version': 'test', 'protocol': 1,
                'features': ['project', 'image-le64'], 'pad': 'x' * 600000}}).encode())
op, body = frame(s)
answer = json.loads(body)
features = answer['response']['response']['features']
assert 'project' in features and not [f for f in features if f.startswith('image')], features
send(s, 8, struct.pack('>H', 1000))
op, body = frame(s)
assert op == 8, op
s.close()

# A client that names no protocol the server speaks is refused at the handshake.
try:
    connect(alpn=['h2']).close()
    raise SystemExit('a client of h2 only was accepted')
except ssl.SSLError:
    pass

# Plain text on the TLS port gets no answer in HTTP, and no hang.
raw = socket.create_connection(('127.0.0.1', port))
raw.settimeout(10 * scale)
raw.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\n')
try:
    got = raw.recv(4096)
except ConnectionResetError:
    got = b''
assert not got.startswith(b'HTTP/'), got
raw.close()
PY

# The server lived through all that, and says nothing of a failure of its own.
run_fyai remote fingerprint
assert_status 0

# A key and a certificate of a CA, for the name of the host, or of another one.
if command -v openssl >/dev/null 2>&1; then
	cd "$TEST_DIR"
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
		-keyout ca.key -out ca.pem -subj /CN=fyai-test-ca -days 2 \
		>/dev/null 2>&1
	make_cert() {
		local name="$1" san="$2"
		openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
			-keyout "$name.key" -out "$name.csr" -subj "/CN=$name" >/dev/null 2>&1
		printf 'subjectAltName=%s\nextendedKeyUsage=serverAuth\n' "$san" >"$name.ext"
		openssl x509 -req -in "$name.csr" -CA ca.pem -CAkey ca.key \
			-CAcreateserial -days 2 -extfile "$name.ext" -out "$name.pem" \
			>/dev/null 2>&1
		chmod 600 "$name.key"
	}
	make_cert good IP:127.0.0.1
	make_cert other DNS:other.example

	start_tls_server() {
		"$FYAI_BIN" serve --socket "$TEST_DIR/$1.sock" --listen https://127.0.0.1:0 \
			--tls-cert "$TEST_DIR/$1.pem" --tls-key "$TEST_DIR/$1.key" \
			>"$TEST_DIR/$1.out" 2>"$TEST_DIR/$1.err" &
		TLS_PID=$!
		# The log says where it listens once it does.
		"$PYTHON" - "$TEST_DIR/$1.err" "$TLS_PID" <<'PY' || fail "the server of $1 did not listen: $(cat "$TEST_DIR/$1.err")"
import os, sys, time
path, pid = sys.argv[1], int(sys.argv[2])
end = time.monotonic() + 10 * int(os.environ.get('FYAI_TIMEOUT_SCALE', '1'))
while 'listening on https' not in open(path).read():
    os.kill(pid, 0)
    assert time.monotonic() < end
PY
		TLS_PORT="$(sed -n 's#.*listening on https://127.0.0.1:\([0-9]*\)/.*#\1#p' "$TEST_DIR/$1.err")"
		[ -n "$TLS_PORT" ] || fail "the server of $1 did not listen: $(cat "$TEST_DIR/$1.err")"
	}

	start_tls_server good
	GOOD_PID=$TLS_PID
	CA_URL="https://127.0.0.1:$TLS_PORT/."
	env -u FYAI_REMOTE_PIN FYAI_REMOTE_CA="$TEST_DIR/ca.pem" "$FYAI_BIN" \
		--remote "$CA_URL" branch list >"$TEST_DIR/ca.out" </dev/null
	same_as_socket "$TEST_DIR/ca.out"
	# The CA of the server is not a root of the system.
	set +e
	env -u FYAI_REMOTE_PIN -u FYAI_REMOTE_CA "$FYAI_BIN" --remote "$CA_URL" \
		branch list >/dev/null 2>"$TEST_DIR/noca.err" </dev/null
	status=$?
	set -e
	[ "$status" -ne 0 ] || fail "a CA that nobody trusts was accepted"
	grep -q 'not trusted: unable to get local issuer certificate' "$TEST_DIR/noca.err" ||
		fail "no CA: $(cat "$TEST_DIR/noca.err")"
	kill "$GOOD_PID"
	wait "$GOOD_PID" 2>/dev/null || true

	start_tls_server other
	OTHER_PID=$TLS_PID
	set +e
	env -u FYAI_REMOTE_PIN FYAI_REMOTE_CA="$TEST_DIR/ca.pem" "$FYAI_BIN" \
		--remote "https://127.0.0.1:$TLS_PORT/." branch list >/dev/null \
		2>"$TEST_DIR/name.err" </dev/null
	status=$?
	set -e
	[ "$status" -ne 0 ] || fail "a certificate for another name was accepted"
	grep -q 'not trusted: IP address mismatch' "$TEST_DIR/name.err" ||
		fail "another name: $(cat "$TEST_DIR/name.err")"
	kill "$OTHER_PID"
	wait "$OTHER_PID" 2>/dev/null || true
	cd - >/dev/null
fi

# A key that others can read is not used, and a certificate needs its key.
chmod 644 "$TEST_DIR/server.pem" 2>/dev/null || cp "$HOME/.fyai/tls/server.pem" "$TEST_DIR/server.pem"
chmod 644 "$TEST_DIR/server.pem"
run_fyai serve --socket "$TEST_DIR/bad.sock" --listen https://127.0.0.1:0 \
	--tls-cert "$TEST_DIR/server.pem" --tls-key "$TEST_DIR/server.pem"
assert_status 1
assert_stderr_contains "must be a file that only you can read"
run_fyai serve --socket "$TEST_DIR/bad.sock" --listen https://127.0.0.1:0 \
	--tls-cert "$TEST_DIR/server.pem"
assert_status 1
assert_stderr_contains "--tls-cert and --tls-key come together"

# A new key has another fingerprint, which a running server does not take.
run_fyai remote rekey --tls-name host.example
assert_status 0
assert_stdout_contains "rekey: the new key is sha256:"
if grep -q "$PIN" <<<"$(cat "$TEST_DIR/stdout")"; then fail "rekey kept the key"; fi

pass
